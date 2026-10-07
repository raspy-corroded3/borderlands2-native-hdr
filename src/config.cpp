#include "config.h"

#include <windows.h>

#include "log.h"

namespace bl2hdr::config {
namespace {
Settings g_settings;

std::wstring IniPath() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  wchar_t* slash = wcsrchr(path, L'\\');
  if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"bl2hdr.ini");
  return path;
}
}  // namespace

void Load() {
  const std::wstring ini = IniPath();
  wchar_t buf[MAX_PATH] = {};
  GetPrivateProfileStringW(L"proxy", L"Chain", L"", buf, MAX_PATH, ini.c_str());
  g_settings.chain = buf;
  GetPrivateProfileStringW(L"d3d9", L"Mode", L"9on12", buf, MAX_PATH, ini.c_str());
  g_settings.mode = buf;
  GetPrivateProfileStringW(L"d3d9", L"Gpu", L"high-performance", buf, MAX_PATH, ini.c_str());
  g_settings.gpu = buf;
  GetPrivateProfileStringW(L"d3d9", L"OnDeviceRemoved", L"message", buf, MAX_PATH, ini.c_str());
  if (_wcsicmp(buf, L"message") != 0 && _wcsicmp(buf, L"exit") != 0 && _wcsicmp(buf, L"continue") != 0) {
    log::Warn("config: unknown [d3d9] OnDeviceRemoved='%ls', using message", buf);
    wcscpy_s(buf, L"message");
  }
  g_settings.on_device_removed = buf;
  GetPrivateProfileStringW(L"shaders", L"Tonemap", L"hdr", buf, MAX_PATH, ini.c_str());
  if (_wcsicmp(buf, L"replica") == 0) {
    g_settings.tonemap = TonemapVariant::kReplica;
  } else if (_wcsicmp(buf, L"tint") == 0) {
    g_settings.tonemap = TonemapVariant::kTint;
  } else if (_wcsicmp(buf, L"hdr") == 0) {
    g_settings.tonemap = TonemapVariant::kHdr;
  } else {
    if (_wcsicmp(buf, L"original") != 0) log::Warn("config: unknown [shaders] Tonemap='%ls', using original", buf);
    g_settings.tonemap = TonemapVariant::kOriginal;
  }
  log::Info("config: [shaders] Tonemap='%ls'", buf);
  GetPrivateProfileStringW(L"output", L"Mode", L"dxgi", buf, MAX_PATH, ini.c_str());
  g_settings.output = buf;
  log::Info("config: [output] Mode='%ls'", buf);
  GetPrivateProfileStringW(L"output", L"Format", L"hdr", buf, MAX_PATH, ini.c_str());
  g_settings.output_hdr = _wcsicmp(buf, L"hdr") == 0;
  GetPrivateProfileStringW(L"hdr", L"PaperWhiteNits", L"203", buf, MAX_PATH, ini.c_str());
  g_settings.paper_white_nits = static_cast<float>(_wtof(buf));
  GetPrivateProfileStringW(L"hdr", L"PeakNits", L"1000", buf, MAX_PATH, ini.c_str());
  g_settings.peak_nits = static_cast<float>(_wtof(buf));
  g_settings.debug_highlights = GetPrivateProfileIntW(L"hdr", L"DebugHighlights", 0, ini.c_str()) != 0;
  g_settings.test_pattern = GetPrivateProfileIntW(L"hdr", L"TestPattern", 0, ini.c_str()) != 0;
  if (g_settings.test_pattern) log::Info("config: [hdr] TestPattern=1 - showing the peak-brightness test pattern");
  GetPrivateProfileStringW(L"hdr", L"Strength", L"1.0", buf, MAX_PATH, ini.c_str());
  g_settings.hdr_strength = static_cast<float>(_wtof(buf));
  if (g_settings.hdr_strength < 0.f) g_settings.hdr_strength = 0.f;
  if (g_settings.hdr_strength > 2.f) g_settings.hdr_strength = 2.f;
  log::Info("config: [hdr] Strength=%.2f", g_settings.hdr_strength);
  g_settings.hdr_enabled = GetPrivateProfileIntW(L"hdr", L"Enabled", 1, ini.c_str()) != 0;
  g_settings.game_hooks = GetPrivateProfileIntW(L"menu", L"GameHooks", 1, ini.c_str()) != 0;
  g_settings.menu_hdr_option = GetPrivateProfileIntW(L"menu", L"HdrOption", 1, ini.c_str()) != 0;
  log::Info("config: [hdr] Enabled=%d, [menu] GameHooks=%d HdrOption=%d", g_settings.hdr_enabled,
            g_settings.game_hooks, g_settings.menu_hdr_option);
  {
    wchar_t cmds[1024] = {};
    GetPrivateProfileStringW(L"debug", L"ConsoleAtSec", L"", cmds, 1024, ini.c_str());
    g_settings.console_at_sec.clear();
    std::wstring all = cmds;
    size_t start = 0;
    while (start < all.size()) {
      size_t end = all.find(L';', start);
      if (end == std::wstring::npos) end = all.size();
      const std::wstring item = all.substr(start, end - start);
      const size_t colon = item.find(L':');
      if (colon != std::wstring::npos) {
        g_settings.console_at_sec.emplace_back(_wtoi(item.substr(0, colon).c_str()), item.substr(colon + 1));
      }
      start = end + 1;
    }
    log::Info("config: [debug] ConsoleAtSec='%ls' (%zu command(s))", cmds, g_settings.console_at_sec.size());
  }
  if (g_settings.paper_white_nits < 40.f || g_settings.paper_white_nits > 1000.f) g_settings.paper_white_nits = 203.f;
  if (g_settings.peak_nits < g_settings.paper_white_nits) g_settings.peak_nits = g_settings.paper_white_nits;
  GetPrivateProfileStringW(L"hdr", L"UIWhiteNits", L"", buf, MAX_PATH, ini.c_str());
  g_settings.ui_white_nits = buf[0] ? static_cast<float>(_wtof(buf)) : g_settings.paper_white_nits;
  if (g_settings.ui_white_nits < 40.f || g_settings.ui_white_nits > 1000.f) g_settings.ui_white_nits = g_settings.paper_white_nits;
  log::Info("config: [hdr] UIWhiteNits=%.0f", g_settings.ui_white_nits);
  log::Info("config: [output] Format=%s, [hdr] PaperWhiteNits=%.0f PeakNits=%.0f DebugHighlights=%d",
            g_settings.output_hdr ? "hdr" : "sdr", g_settings.paper_white_nits, g_settings.peak_nits,
            g_settings.debug_highlights);
  g_settings.force_borderless = GetPrivateProfileIntW(L"display", L"ForceBorderless", 1, ini.c_str()) != 0;
  log::Info("config: [display] ForceBorderless=%d", g_settings.force_borderless);
  g_settings.log_shaders = GetPrivateProfileIntW(L"debug", L"LogShaders", 0, ini.c_str()) != 0;
  g_settings.stats_interval_sec = GetPrivateProfileIntW(L"debug", L"StatsIntervalSec", 0, ini.c_str());
  g_settings.trace_frame_at_sec = GetPrivateProfileIntW(L"debug", L"TraceFrameAtSec", 0, ini.c_str());
  log::Info("config: [debug] TraceFrameAtSec=%d", g_settings.trace_frame_at_sec);
  GetPrivateProfileStringW(L"debug", L"CaptureAtSec", L"", buf, MAX_PATH, ini.c_str());
  g_settings.capture_at_sec.clear();
  for (wchar_t* p = buf; *p;) {
    wchar_t* end = nullptr;
    const long v = wcstol(p, &end, 10);
    if (end == p) { ++p; continue; }
    if (v > 0) g_settings.capture_at_sec.push_back(static_cast<int>(v));
    p = end;
  }
  g_settings.quit_after_sec = GetPrivateProfileIntW(L"debug", L"QuitAfterSec", 0, ini.c_str());
  g_settings.minimize_at_sec = GetPrivateProfileIntW(L"debug", L"MinimizeAtSec", 0, ini.c_str());
  g_settings.restore_at_sec = GetPrivateProfileIntW(L"debug", L"RestoreAtSec", 0, ini.c_str());
  g_settings.remove_device_at_sec = GetPrivateProfileIntW(L"debug", L"RemoveDeviceAtSec", 0, ini.c_str());
  if (g_settings.remove_device_at_sec > 0) {
    log::Warn("config: [debug] RemoveDeviceAtSec=%d - the GPU device will be removed on purpose",
              g_settings.remove_device_at_sec);
  }
  log::Info("config: [debug] CaptureAtSec='%ls' (%zu) QuitAfterSec=%d", buf, g_settings.capture_at_sec.size(),
            g_settings.quit_after_sec);
  log::Info("config: Chain='%ls' Mode='%ls' LogShaders=%d StatsIntervalSec=%d (ini %ls)", g_settings.chain.c_str(),
            g_settings.mode.c_str(), g_settings.log_shaders, g_settings.stats_interval_sec,
            GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES ? L"not found, defaults" : ini.c_str());
}

const Settings& Get() { return g_settings; }
}  // namespace bl2hdr::config

namespace bl2hdr::process {
namespace {
bool g_is_game = false;
}
void SetIsGame(bool is_game) { g_is_game = is_game; }
bool IsGame() { return g_is_game; }
}  // namespace bl2hdr::process
