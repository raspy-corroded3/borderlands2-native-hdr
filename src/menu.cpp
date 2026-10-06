// "HDR: Off / On" in BL2's Video options (milestone 5).
//
// How the game builds the menu (from the game's UnrealScript class declarations, WillowGame package):
//   WillowScrollingListDataProviderVideoOptions.Populate(WillowScrollingList TheList) fills the list
//   with AddSpinnerListItem(EventID, Caption, bDisabled, StartingChoiceIndex, Choices) etc. Populate is
//   called script-to-script (CallFunction), so we add our row in a CallFunction *post* hook; the list
//   is the calling object (FFrame.Object), validated by class name.
//   Spinner changes arrive from Flash as WillowScrollingList.OnSpinnerValueChange(EntryIndex,
//   NewChoiceIndex, NewChoiceValue) via ProcessEvent; the row's EventID is IndexToEventId[EntryIndex].
#include "menu.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"
#include "ue3.h"

namespace bl2hdr::menu {
namespace {
constexpr int32_t kHdrEventId = 9417;  // unused by the game's own options

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_active{false};
long long g_first_qpc = 0;

ue3::UFunction* g_populate = nullptr;        // VideoOptions.Populate
ue3::UFunction* g_on_spinner = nullptr;      // WillowScrollingList.OnSpinnerValueChange
ue3::UFunction* g_add_spinner = nullptr;     // WillowScrollingList.AddSpinnerListItem
ue3::UFunction* g_add_description = nullptr; // OptionsBase.AddDescription
ue3::UFunction* g_console_command = nullptr; // PlayerController.ConsoleCommand
int32_t g_off_entry_index = -1, g_off_choice_index = -1;
size_t g_next_console = 0;

double Seconds() {
  if (!g_first_qpc) return 0.0;
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  return static_cast<double>(now.QuadPart - g_first_qpc) / static_cast<double>(freq.QuadPart);
}

void SaveEnabled(bool on) {
  wchar_t ini[MAX_PATH];
  GetModuleFileNameW(nullptr, ini, MAX_PATH);
  if (wchar_t* slash = wcsrchr(ini, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), L"bl2hdr.ini");
  WritePrivateProfileStringW(L"hdr", L"Enabled", on ? L"1" : L"0", ini);
}

void AddHdrRow(ue3::UObject* list, ue3::UObject* provider) {
  if (!g_add_spinner) return;
  ue3::Params p(reinterpret_cast<ue3::UFunction*>(g_add_spinner));
  bool ok = p.SetInt("EventID", kHdrEventId) && p.SetString("Caption", L"HDR") && p.SetBool("bDisabled", false) &&
            p.SetInt("StartingChoiceIndex", g_enabled ? 1 : 0) && p.SetStringArray("Choices", {L"Off", L"On"});
  if (!ok) {
    log::Error("menu: AddSpinnerListItem parameters not as expected (%s) - not adding the HDR row", p.Describe().c_str());
    return;
  }
  ue3::Call(list, g_add_spinner, p.data());
  if (g_add_description && provider) {
    ue3::Params d(g_add_description);
    if (d.SetInt("EventID", kHdrEventId) &&
        d.SetString("Description", L"High dynamic range output (bl2hdr). Brightness and peak are set in bl2hdr.ini.")) {
      ue3::Call(provider, g_add_description, d.data());
    }
  }
  log::Info("menu: added 'HDR: %s' to the Video options (list %p, provider %p)", g_enabled ? "On" : "Off",
            static_cast<void*>(list), static_cast<void*>(provider));
}

void RunConsoleCommand(const std::wstring& cmd) {
  ue3::UObject* pc = ue3::FindInstance("WillowPlayerController");
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

// ---- hooks (game thread) ----
bool OnProcessEvent(ue3::UObject* obj, ue3::UFunction* fn, void* params, bool post) {
  if (g_need_resolve.exchange(false)) Resolve();  // object lookups happen on the game thread
  if (!post) {
    // Timed console commands for automated tests ([debug] ConsoleAtSec), run on the game thread.
    const auto& cmds = config::Get().console_at_sec;
    if (g_next_console < cmds.size() && Seconds() >= cmds[g_next_console].first) {
      RunConsoleCommand(cmds[g_next_console++].second);
    }
  }
  if (fn == g_on_spinner && !post && params) {
    const int32_t entry = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(params) + g_off_entry_index);
    const int32_t choice = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(params) + g_off_choice_index);
    std::vector<int32_t> ids;
    if (ue3::ReadIntArray(obj, "IndexToEventId", &ids) && entry >= 0 && entry < static_cast<int32_t>(ids.size()) &&
        ids[entry] == kHdrEventId) {
      g_enabled = choice == 1;
      SaveEnabled(g_enabled);
      log::Info("menu: HDR switched %s from the Video options (row %d)", g_enabled ? "ON" : "OFF", entry);
      return true;  // ours: do not pass an unknown EventID to the game's option handler
    }
  }
  if (fn == g_populate && post && params) {  // in case Populate is ever called natively
    AddHdrRow(*reinterpret_cast<ue3::UObject**>(params), obj);
  }
  return false;
}

void OnCallFunction(ue3::UObject* obj, ue3::UFunction* fn, ue3::FFrame* stack, bool post) {
  if (fn != g_populate || !post) return;
  ue3::UObject* list = ue3::CallerObject(stack);
  const std::string cls = ue3::ClassName(list);
  if (cls != "WillowScrollingList") {
    log::Warn("menu: Populate caller is '%s' (%s), not WillowScrollingList - HDR row not added", cls.c_str(),
              ue3::PathName(list).c_str());
    return;
  }
  AddHdrRow(list, obj);
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
  ue3::SelfCheck();
  g_populate = ue3::FindFunction("WillowGame.WillowScrollingListDataProviderVideoOptions.Populate");
  g_on_spinner = ue3::FindFunction("WillowGame.WillowScrollingList.OnSpinnerValueChange");
  g_add_spinner = ue3::FindFunction("WillowGame.WillowScrollingList.AddSpinnerListItem");
  g_add_description = ue3::FindFunction("WillowGame.WillowScrollingListDataProviderOptionsBase.AddDescription");
  g_console_command = ue3::FindFunction("Engine.PlayerController.ConsoleCommand");
  g_off_entry_index = ue3::ParamOffset(g_on_spinner, "EntryIndex");
  g_off_choice_index = ue3::ParamOffset(g_on_spinner, "NewChoiceIndex");
  if (g_add_spinner) log::Info("menu: AddSpinnerListItem params: %s", ue3::Params(g_add_spinner).Describe().c_str());
  if (g_on_spinner) log::Info("menu: OnSpinnerValueChange params: %s", ue3::Params(g_on_spinner).Describe().c_str());
  const bool menu_ok = g_populate && g_on_spinner && g_add_spinner && g_off_entry_index >= 0 && g_off_choice_index >= 0;
  if (!config::Get().menu_hdr_option) {
    g_populate = g_on_spinner = nullptr;  // option disabled: keep only console automation
  } else if (!menu_ok) {
    log::Error("menu: required functions not found - HDR option disabled");
    g_populate = g_on_spinner = nullptr;
  }
  g_active = true;
  log::Info("menu: active (HDR option %s, HDR currently %s)", g_populate ? "on" : "off", g_enabled ? "ON" : "OFF");
}
}  // namespace

}  // namespace bl2hdr::menu
