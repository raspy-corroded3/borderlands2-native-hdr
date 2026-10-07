// "HDR" in BL2's Video options: a row that opens an HDR settings page (HDR on/off and brightness
// sliders, applied live and saved to bl2hdr.ini), or a plain "HDR: Off / On" row as a fallback.
//
// How the game builds the menu (from the game's UnrealScript class declarations, WillowGame package):
//   WillowScrollingListDataProviderVideoOptions.Populate(WillowScrollingList TheList) fills the list
//   with AddSpinnerListItem / AddSliderListItem / AddListItem(EventID, Caption, ...). Populate is called
//   script-to-script (CallFunction), so rows are added in a CallFunction *post* hook; the list is the
//   calling object (FFrame.Object), validated by class name.
//   Spinner, slider and click events arrive from Flash on the list via ProcessEvent:
//   OnSpinnerValueChange(EntryIndex, NewChoiceIndex, NewChoiceValue), OnSliderValueChange(EntryIndex,
//   NewValue) and OnClikEvent(EventData Data: Type, Data, mouseIndex, Button, Index, ...); the row's
//   EventID is IndexToEventId[EntryIndex or Data.Index]. Ours are handled here and not passed on.
//   Sub-pages are data providers pushed with WillowScrollingList.PushDataProvider. The HDR page is a new
//   WillowScrollingListDataProviderOptionsBase (UObject::StaticConstructObject, see ue3.h): the game's
//   own options page base, so descriptions and Back work as on the other pages; its Populate is
//   followed by our rows.
#include "menu.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"
#include "ue3.h"

namespace bl2hdr::menu {
namespace {
// The game's WillowScrollingListDataProviderVideoOptions.EVENT_ID_WINDOW_MODE (WillowGame.upk Const "5001").
constexpr int32_t kWindowModeEventId = 5001;
// EventIDs unused by the game's own options.
constexpr int32_t kHdrEventId = 9417;  // Video options row: opens the HDR page (or the fallback spinner)
constexpr int32_t kRowEnabled = 9418;  // HDR page rows
constexpr int32_t kRowGame = 9419;
constexpr int32_t kRowUi = 9420;
constexpr int32_t kRowVideo = 9421;
constexpr int32_t kRowPeak = 9422;
constexpr int32_t kRowStrength = 9423;

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_active{false};
long long g_first_qpc = 0;

ue3::UFunction* g_populate = nullptr;         // VideoOptions.Populate
ue3::UFunction* g_on_spinner = nullptr;       // WillowScrollingList.OnSpinnerValueChange
ue3::UFunction* g_on_slider = nullptr;        // WillowScrollingList.OnSliderValueChange
ue3::UFunction* g_on_clik = nullptr;          // WillowScrollingList.OnClikEvent
ue3::UFunction* g_add_spinner = nullptr;      // WillowScrollingList.AddSpinnerListItem
ue3::UFunction* g_add_slider = nullptr;       // WillowScrollingList.AddSliderListItem
ue3::UFunction* g_add_item = nullptr;         // WillowScrollingList.AddListItem
ue3::UFunction* g_push_provider = nullptr;    // WillowScrollingList.PushDataProvider
ue3::UFunction* g_add_description = nullptr;  // OptionsBase.AddDescription
ue3::UFunction* g_page_populate = nullptr;    // OptionsBase.Populate (the HDR page's)
ue3::UFunction* g_page_on_pop = nullptr;      // OptionsBase.OnPop
ue3::UObject* g_page_class = nullptr;         // WillowScrollingListDataProviderOptionsBase
ue3::UFunction* g_console_command = nullptr;  // PlayerController.ConsoleCommand
int32_t g_off_entry_index = -1, g_off_choice_index = -1;  // OnSpinnerValueChange
int32_t g_off_slider_entry = -1, g_off_slider_value = -1;  // OnSliderValueChange
int32_t g_off_clik_data = -1;                              // OnClikEvent
size_t g_next_console = 0;
bool g_use_page = false;               // HDR page available (else the fallback spinner row)
ue3::UObject* g_video_provider = nullptr;  // the Video options provider that got our row
ue3::UObject* g_page = nullptr;            // the HDR page while it is pushed (game thread)
bool g_in_video_populate = false;          // inside the Video options' Populate (game thread)
bool g_video_row_added = false;            // our row was added during this Populate

double Seconds() {
  if (!g_first_qpc) return 0.0;
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  return static_cast<double>(now.QuadPart - g_first_qpc) / static_cast<double>(freq.QuadPart);
}

int32_t ReadInt(void* params, int32_t offset) {
  return *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(params) + offset);
}

// EventID of list row `entry`, or -1.
int32_t RowEventId(ue3::UObject* list, int32_t entry) {
  std::vector<int32_t> ids;
  if (!ue3::ReadIntArray(list, "IndexToEventId", &ids) || entry < 0 || entry >= static_cast<int32_t>(ids.size())) {
    return -1;
  }
  return ids[entry];
}

void SaveEnabled(bool on) { config::SaveHdr(L"Enabled", on ? 1.0f : 0.0f); }

void AddDescription(ue3::UObject* provider, int32_t id, const wchar_t* text) {
  if (!g_add_description || !provider) return;
  ue3::Params d(g_add_description);
  if (d.SetInt("EventID", id) && d.SetString("Description", text)) ue3::Call(provider, g_add_description, d.data());
}

bool AddSpinner(ue3::UObject* list, int32_t id, const wchar_t* caption, int32_t choice,
                const std::vector<std::wstring>& choices) {
  ue3::Params p(g_add_spinner);
  if (!(p.SetInt("EventID", id) && p.SetString("Caption", caption) && p.SetBool("bDisabled", false) &&
        p.SetInt("StartingChoiceIndex", choice) && p.SetStringArray("Choices", choices))) {
    log::Error("menu: AddSpinnerListItem parameters not as expected (%s)", p.Describe().c_str());
    return false;
  }
  ue3::Call(list, g_add_spinner, p.data());
  return true;
}

// ---- the HDR page ----
// Sliders send whole numbers: nits, or percent for the strength.
struct SliderRow {
  int32_t id;
  const wchar_t* caption;
  const wchar_t* description;
  float min, max, step;
  const wchar_t* ini_key;
};
const SliderRow kSliders[] = {
    {kRowGame, L"GAME BRIGHTNESS", L"Brightness of white in the 3D game world, in nits. 200-300 is typical.",
     config::kMinWhiteNits, config::kMaxWhiteNits, 10.0f, L"PaperWhiteNits"},
    {kRowUi, L"HUD AND MENU BRIGHTNESS", L"Brightness of the HUD, menus and loading screens, in nits.",
     config::kMinWhiteNits, config::kMaxWhiteNits, 10.0f, L"UIWhiteNits"},
    {kRowVideo, L"CUTSCENE BRIGHTNESS", L"Brightness of cutscene videos, in nits.", config::kMinWhiteNits,
     config::kMaxWhiteNits, 10.0f, L"VideoWhiteNits"},
    {kRowPeak, L"PEAK BRIGHTNESS",
     L"Your display's peak brightness in nits; highlights stop here. Measure it with TestPattern=1 in bl2hdr.ini.",
     config::kMinPeakNits, config::kMaxPeakNits, 50.0f, L"PeakNits"},
    {kRowStrength, L"HIGHLIGHT STRENGTH",
     L"How far highlights go above SDR white, in percent: 0 = SDR look, 100 = normal.", 0.0f, 200.0f, 10.0f,
     L"Strength"},
};

float* SliderSetting(int32_t id) {
  auto& s = config::Mutable();
  switch (id) {
    case kRowGame: return &s.paper_white_nits;
    case kRowUi: return &s.ui_white_nits;
    case kRowVideo: return &s.video_white_nits;
    case kRowPeak: return &s.peak_nits;
    case kRowStrength: return &s.hdr_strength;
    default: return nullptr;
  }
}

const SliderRow* FindSlider(int32_t id) {
  for (const auto& r : kSliders) {
    if (r.id == id) return &r;
  }
  return nullptr;
}

void AddPageRows(ue3::UObject* list) {
  AddSpinner(list, kRowEnabled, L"HDR", g_enabled ? 1 : 0, {L"Off", L"On"});
  AddDescription(g_page, kRowEnabled, L"High dynamic range output. Off keeps the brightness settings but gives the SDR look.");
  for (const auto& r : kSliders) {
    const float* setting = SliderSetting(r.id);
    const float shown = r.id == kRowStrength ? *setting * 100.0f : *setting;  // strength: percent
    ue3::Params p(g_add_slider);
    if (!(p.SetInt("EventID", r.id) && p.SetString("Caption", r.caption) && p.SetBool("bDisabled", false) &&
          p.SetFloat("StartingValue", std::clamp(std::round(shown), r.min, r.max)) && p.SetFloat("MinValue", r.min) &&
          p.SetFloat("MaxValue", r.max) && p.SetFloat("Increment", r.step))) {
      log::Error("menu: AddSliderListItem parameters not as expected (%s)", p.Describe().c_str());
      return;
    }
    ue3::Call(list, g_add_slider, p.data());
    AddDescription(g_page, r.id, r.description);
  }
  log::Info("menu: HDR page rows added (list %p, page %p)", static_cast<void*>(list), static_cast<void*>(g_page));
}

void OpenHdrPage(ue3::UObject* list) {
  ue3::UObject* outer = g_video_provider ? g_video_provider : list;
  ue3::UObject* page = ue3::Construct(g_page_class, outer);
  if (!page) {
    log::Error("menu: could not create the HDR page");
    return;
  }
  // Descriptions are shown through the options movie the game's own pages use.
  ue3::UObject* movie = nullptr;
  if (g_video_provider && ue3::GetObjectMember(g_video_provider, "MyOptionsMovie", &movie)) {
    ue3::SetObjectMember(page, "MyOptionsMovie", movie);
  }
  ue3::SetStringMember(page, "MenuDisplayName", L"HDR");  // the "PAUSE / OPTIONS / VIDEO / HDR" title
  ue3::Params p(g_push_provider);
  if (!p.SetInterface("DataProvider", page)) {
    log::Error("menu: PushDataProvider parameters not as expected (%s)", p.Describe().c_str());
    return;
  }
  g_page = page;
  ue3::Call(list, g_push_provider, p.data(), true);  // its Populate call must reach OnCallFunction
  log::Info("menu: HDR page opened (%s)", ue3::PathName(page).c_str());
}

void OnSlider(int32_t id, int32_t value) {
  const SliderRow* r = FindSlider(id);
  float* setting = SliderSetting(id);
  if (!r || !setting) return;
  const float v = std::clamp(static_cast<float>(value), r->min, r->max);
  if (id == kRowStrength) {
    *setting = v / 100.0f;
    config::SaveHdr(r->ini_key, *setting, 2);
  } else {
    *setting = v;
    config::SaveHdr(r->ini_key, v);
  }
  log::Info("menu: %ls = %d", r->ini_key, value);
}

// ---- Video options row ----
void AddHdrRow(ue3::UObject* list, ue3::UObject* provider) {
  g_video_provider = provider;
  g_video_row_added = true;
  if (g_use_page) {
    ue3::Params p(g_add_item);
    if (!(p.SetInt("EventID", kHdrEventId) && p.SetString("Caption", L"HDR") && p.SetBool("bDisabled", false))) {
      log::Error("menu: AddListItem parameters not as expected (%s) - not adding the HDR row", p.Describe().c_str());
      return;
    }
    ue3::Call(list, g_add_item, p.data());
    AddDescription(provider, kHdrEventId, L"HDR settings (bl2hdr): on/off, brightness and peak.");
  } else {
    if (!g_add_spinner || !AddSpinner(list, kHdrEventId, L"HDR", g_enabled ? 1 : 0, {L"Off", L"On"})) return;
    AddDescription(provider, kHdrEventId, L"High dynamic range output (bl2hdr). Brightness and peak are set in bl2hdr.ini.");
  }
  log::Info("menu: added the HDR %s to the Video options (list %p, provider %p)", g_use_page ? "page row" : "spinner",
            static_cast<void*>(list), static_cast<void*>(provider));
}

// ---- test commands ----
// Test-only pseudo commands (not console commands): "!movie <name>" plays a Bink movie from
// WillowGame\Movies through GamePlayerController.ClientPlayMovie (the game's own cutscene path),
// "!moviestop" stops it.
bool RunMovieCommand(ue3::UObject* pc, const std::wstring& cmd) {
  if (cmd.rfind(L"!movie", 0) != 0) return false;
  const bool stop = cmd == L"!moviestop";
  const size_t start = cmd.find_first_not_of(L' ', 6);
  const std::wstring name = stop || start == std::wstring::npos ? L"" : cmd.substr(start);
  ue3::UFunction* fn = ue3::FindFunction(stop ? "GameFramework.GamePlayerController.ClientStopMovie"
                                              : "GameFramework.GamePlayerController.ClientPlayMovie");
  if (!fn || (!stop && name.empty())) {
    log::Warn("menu: '%ls' skipped (function %p)", cmd.c_str(), static_cast<void*>(fn));
    return true;
  }
  ue3::Params p(fn);
  bool ok;
  if (stop) {
    ok = p.SetBool("bForceStopNonSkippable", true);
  } else {
    ok = p.SetString("MovieName", name) && p.SetInt("InStartOfRenderingMovieFrame", -1) &&
         p.SetInt("InEndOfRenderingMovieFrame", -1) && p.SetBool("bPlayOnceFromStream", true);
  }
  if (!ok) {
    log::Error("menu: '%ls' parameters not as expected (%s)", cmd.c_str(), p.Describe().c_str());
    return true;
  }
  ue3::Call(pc, fn, p.data());
  log::Info("menu: '%ls' called %s", cmd.c_str(), stop ? "ClientStopMovie" : "ClientPlayMovie");
  return true;
}

// "!echo <event tag path> <name tag path>" plays an ECHO recording through
// WillowDialogManager.PlayEchoDialog (the game's own path; video ECHOs show a TextureMovie portrait), e.g.
// "!echo GD_DialogEpisode2.Events.VO_Ep2_Pt1_01_echo_Angel GD_Dialog_NPC.Names.DialogName_Angel".
void RunEchoCommand(const std::wstring& cmd) {
  std::string args;  // object paths are ASCII
  for (size_t i = 6; i < cmd.size(); ++i) args += static_cast<char>(cmd[i]);
  const size_t space = args.find(' ');
  const std::string event_path = args.substr(0, space);
  const std::string name_path = space == std::string::npos ? "" : args.substr(args.find_first_not_of(' ', space));
  ue3::UObject* manager = ue3::FindInstance("WillowDialogManager");
  ue3::UFunction* fn = ue3::FindFunction("WillowGame.WillowDialogManager.PlayEchoDialog");
  ue3::UObject* event = ue3::FindObject("WillowDialogEventTag", event_path);
  ue3::UObject* name = name_path.empty() ? nullptr : ue3::FindObject("WillowDialogNameTag", name_path);
  if (!manager || !fn || !event || (!name_path.empty() && !name)) {
    log::Warn("menu: '%ls' skipped (manager %p, function %p, event %p, name %p)", cmd.c_str(),
              static_cast<void*>(manager), static_cast<void*>(fn), static_cast<void*>(event), static_cast<void*>(name));
    return;
  }
  ue3::Params p(fn);
  if (!p.SetObject("InEvent", event) || !p.SetObject("InName", name) || !p.SetBool("bForcePlayAsPureEcho", false)) {
    log::Error("menu: '%ls' parameters not as expected (%s)", cmd.c_str(), p.Describe().c_str());
    return;
  }
  ue3::Call(manager, fn, p.data());
  log::Info("menu: '%ls' called PlayEchoDialog", cmd.c_str());
}

void RunConsoleCommand(const std::wstring& cmd) {
  if (cmd.rfind(L"!echo ", 0) == 0) {
    RunEchoCommand(cmd);
    return;
  }
  ue3::UObject* pc = ue3::FindInstance("WillowPlayerController");
  if (pc && RunMovieCommand(pc, cmd)) return;
  if (!pc || !g_console_command) {
    log::Warn("menu: console '%ls' skipped (player controller %p, function %p)", cmd.c_str(), static_cast<void*>(pc),
              static_cast<void*>(g_console_command));
    return;
  }
  ue3::Params p(g_console_command);
  p.SetString("Command", cmd);
  ue3::Call(pc, g_console_command, p.data());
  const std::wstring result = p.TakeReturnString();
  log::Info("menu: console '%ls' -> '%ls'", cmd.c_str(), result.c_str());
}

void Resolve();
std::atomic<bool> g_need_resolve{false};
// Game thread only: when a failed lookup is tried again (0 = no retry pending), and how many tries remain.
constexpr double kRetryDelaySec = 10.0;
double g_retry_at = 0.0;
int g_retries_left = 5;

// ---- hooks (game thread) ----
bool OnProcessEvent(ue3::UObject* obj, ue3::UFunction* fn, void* params, bool post) {
  // Object lookups happen on the game thread.
  if (g_need_resolve.exchange(false) || (g_retry_at > 0.0 && Seconds() >= g_retry_at)) Resolve();
  if (!post) {
    // Timed console commands for automated tests ([debug] ConsoleAtSec), run on the game thread.
    const auto& cmds = config::Get().console_at_sec;
    if (g_next_console < cmds.size() && Seconds() >= cmds[g_next_console].first) {
      RunConsoleCommand(cmds[g_next_console++].second);
    }
  }
  if (post || !params) {
    if (fn == g_populate && post && params) AddHdrRow(*reinterpret_cast<ue3::UObject**>(params), obj);  // if ever native
    return false;
  }
  // Our rows' events are handled here and blocked: the game's handlers do not know these EventIDs.
  if (fn == g_on_spinner) {
    const int32_t id = RowEventId(obj, ReadInt(params, g_off_entry_index));
    if (id == kHdrEventId || id == kRowEnabled) {
      g_enabled = ReadInt(params, g_off_choice_index) == 1;
      SaveEnabled(g_enabled);
      log::Info("menu: HDR switched %s from the %s", g_enabled ? "ON" : "OFF", id == kRowEnabled ? "HDR page" : "Video options");
      return true;
    }
  } else if (fn == g_on_slider) {
    const int32_t id = RowEventId(obj, ReadInt(params, g_off_slider_entry));
    if (FindSlider(id)) {
      OnSlider(id, ReadInt(params, g_off_slider_value));
      return true;
    }
  } else if (fn == g_on_clik) {
    // EventData: Type (FString, 12 bytes), Data, mouseIndex, Button, Index (+24), lastIndex, controllerIdx.
    // "change" is the selection moving onto a row: the game's handler updates the description, so it
    // passes. Any other type on the HDR row opens the page.
    const int32_t index = ReadInt(params, g_off_clik_data + 24);
    const int32_t id = RowEventId(obj, index);
    const auto* type = *reinterpret_cast<const wchar_t* const*>(static_cast<uint8_t*>(params) + g_off_clik_data);
    const std::wstring kind = type ? type : L"";
    if (id == kHdrEventId && g_use_page && kind != L"change") {
      log::Info("menu: '%ls' on the HDR row %d", kind.c_str(), index);
      if (!g_page) OpenHdrPage(obj);
      return true;
    }
  }
  return false;
}

void OnCallFunction(ue3::UObject* obj, ue3::UFunction* fn, ue3::FFrame* stack, bool post) {
  // Our row goes right after the game's "Window Mode" row: after each AddListItem made by the Video
  // options' Populate, look at the row just added (end of IndexToEventId). If that row never comes,
  // the row is added at the end when Populate returns.
  if (fn == g_populate) {
    g_in_video_populate = !post;
    if (!post) {
      g_video_row_added = false;
      return;
    }
  } else if (fn == g_add_item && post && g_in_video_populate && !g_video_row_added) {
    std::vector<int32_t> ids;
    if (ue3::ReadIntArray(obj, "IndexToEventId", &ids) && !ids.empty() && ids.back() == kWindowModeEventId) {
      AddHdrRow(obj, ue3::CallerObject(stack));
    }
    return;
  }
  if (!post) return;
  if (fn == g_page_on_pop && obj == g_page && g_page) {
    log::Info("menu: HDR page closed");
    g_page = nullptr;
    return;
  }
  const bool video = fn == g_populate;
  const bool page = fn == g_page_populate && obj == g_page && g_page;
  if (!video && !page) return;
  ue3::UObject* list = ue3::CallerObject(stack);
  const std::string cls = ue3::ClassName(list);
  if (cls != "WillowScrollingList") {
    log::Warn("menu: Populate caller is '%s' (%s), not WillowScrollingList - HDR rows not added", cls.c_str(),
              ue3::PathName(list).c_str());
    return;
  }
  if (video) {
    if (!g_video_row_added) AddHdrRow(list, obj);
  } else {
    AddPageRows(list);
  }
}
}  // namespace

void SetTimeBase(long long first_present_qpc) { g_first_qpc = first_present_qpc; }

bool HdrEnabled() { return g_enabled.load(std::memory_order_relaxed); }

void OnFirstPresent() {
  g_enabled = config::Get().hdr_enabled;
  if (!ue3::Ready()) return;
  // Hooks go live now; the first ProcessEvent call (game thread) resolves the functions.
  g_need_resolve = true;
  ue3::SetProcessEventHook(&OnProcessEvent);
  ue3::SetCallFunctionHook(&OnCallFunction);
}

namespace {
void Resolve() {
  g_retry_at = 0.0;
  ue3::SelfCheck();
  const std::string list = "WillowGame.WillowScrollingList.";
  const std::string base = "WillowGame.WillowScrollingListDataProviderOptionsBase.";
  g_populate = ue3::FindFunction("WillowGame.WillowScrollingListDataProviderVideoOptions.Populate");
  g_on_spinner = ue3::FindFunction(list + "OnSpinnerValueChange");
  g_on_slider = ue3::FindFunction(list + "OnSliderValueChange");
  g_on_clik = ue3::FindFunction(list + "OnClikEvent");
  g_add_spinner = ue3::FindFunction(list + "AddSpinnerListItem");
  g_add_slider = ue3::FindFunction(list + "AddSliderListItem");
  g_add_item = ue3::FindFunction(list + "AddListItem");
  g_push_provider = ue3::FindFunction(list + "PushDataProvider");
  g_add_description = ue3::FindFunction(base + "AddDescription");
  g_page_populate = ue3::FindFunction(base + "Populate");
  g_page_on_pop = ue3::FindFunction(base + "OnPop");
  g_page_class = ue3::FindObject("Class", "WillowGame.WillowScrollingListDataProviderOptionsBase");
  g_console_command = ue3::FindFunction("Engine.PlayerController.ConsoleCommand");
  g_off_entry_index = ue3::ParamOffset(g_on_spinner, "EntryIndex");
  g_off_choice_index = ue3::ParamOffset(g_on_spinner, "NewChoiceIndex");
  g_off_slider_entry = ue3::ParamOffset(g_on_slider, "EntryIndex");
  g_off_slider_value = ue3::ParamOffset(g_on_slider, "NewValue");
  g_off_clik_data = ue3::ParamOffset(g_on_clik, "Data");
  if (g_add_spinner) log::Info("menu: AddSpinnerListItem params: %s", ue3::Params(g_add_spinner).Describe().c_str());
  if (g_add_slider) log::Info("menu: AddSliderListItem params: %s", ue3::Params(g_add_slider).Describe().c_str());
  if (g_on_clik) log::Info("menu: OnClikEvent params: %s", ue3::Params(g_on_clik).Describe().c_str());
  const bool menu_ok = g_populate && g_on_spinner && g_add_spinner && g_off_entry_index >= 0 && g_off_choice_index >= 0;
  // The page also needs the click/slider events, the list functions and object construction. EventData
  // must hold at least Index (+24): the whole struct lies inside the parameter block (ParamOffset checks).
  g_use_page = menu_ok && g_on_slider && g_on_clik && g_add_slider && g_add_item && g_push_provider &&
               g_page_populate && g_page_on_pop && g_page_class && ue3::CanConstruct() && g_off_slider_entry >= 0 &&
               g_off_slider_value >= 0 && g_off_clik_data >= 0 &&
               ue3::Params(g_on_clik).Describe().find("Data:StructProperty@0(36)") != std::string::npos;
  if (!config::Get().menu_hdr_option) {
    g_populate = g_on_spinner = g_on_slider = g_on_clik = nullptr;  // option disabled: keep only console automation
    g_use_page = false;
  } else if (!menu_ok) {
    g_populate = g_on_spinner = g_on_slider = g_on_clik = nullptr;
    g_use_page = false;
    if (g_retries_left-- > 0) {
      log::Warn("menu: required functions not found yet - trying again in %.0f s", kRetryDelaySec);
      g_retry_at = Seconds() + kRetryDelaySec;
    } else {
      log::Error("menu: required functions not found - HDR option disabled");
    }
  }
  g_active = true;
  log::Info("menu: active (HDR option %s, %s, HDR currently %s)", g_populate ? "on" : "off",
            g_use_page ? "settings page" : "Off/On row", g_enabled ? "ON" : "OFF");
}
}  // namespace

}  // namespace bl2hdr::menu
