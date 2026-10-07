#pragma once
// Settings from <exe dir>\bl2hdr.ini. Missing file or keys fall back to the defaults below.
#include <string>
#include <vector>

namespace bl2hdr::config {
enum class TonemapVariant { kOriginal, kReplica, kTint, kHdr };

struct Settings {
  // [shaders] Tonemap: hdr (default: HDR highlight extension) | original (game's shader) |
  // replica (our equivalent rewrite) | tint (visible test)
  TonemapVariant tonemap = TonemapVariant::kHdr;
  // [proxy] Chain: d3d9 implementation to forward to, relative to the exe dir (e.g. dxvk_d3d9.dll).
  // Empty = the system d3d9.dll.
  std::wstring chain;
  // [d3d9] Mode: "9on12" (default: run D3D9 on top of D3D12 via the system Direct3DCreate9On12; only
  // available when chaining to the system d3d9.dll; required for the HDR output) or "native".
  std::wstring mode = L"9on12";
  bool Use9On12() const { return _wcsicmp(mode.c_str(), L"9on12") == 0; }
  // [d3d9] Gpu: graphics card for 9on12 - "high-performance" (default: the fastest GPU, e.g. the dedicated
  // one in a laptop with two), "minimum-power" (the integrated one) or "system" (9on12's own choice).
  std::wstring gpu = L"high-performance";
  // [d3d9] OnDeviceRemoved: "message" (default: explain, then close the game), "exit" (close without a
  // message) or "continue" (only log). 9on12 cannot recover a removed GPU device; the game would hang.
  std::wstring on_device_removed = L"message";
  // [output] Mode: "dxgi" (default: our swapchain via 9on12 interop) or "d3d9" (game's own Present).
  std::wstring output = L"dxgi";
  bool UseDxgiOutput() const { return _wcsicmp(output.c_str(), L"dxgi") == 0; }
  // [output] Format: "sdr" (8-bit copy of the real back buffer) or "hdr" (FP16 substitute back
  // buffer -> FP16 scRGB swapchain with the encode pass). Only with Mode=dxgi.
  bool output_hdr = true;
  // [hdr] brightness settings (nits) and debug view.
  float paper_white_nits = 203.0f;  // brightness of SDR white in the 3D scene ("game brightness")
  float ui_white_nits = 203.0f;     // [hdr] UIWhiteNits: HUD/menus/2D screens (default = PaperWhiteNits)
  float video_white_nits = 203.0f;  // [hdr] VideoWhiteNits: Bink cutscenes (default = UIWhiteNits)
  float peak_nits = 1000.0f;        // safety clamp for the output
  bool debug_highlights = false;    // show values above 1.0 (beyond SDR) in magenta
  bool test_pattern = false;        // [hdr] TestPattern: show the peak-brightness test pattern instead of the game
  float hdr_strength = 1.0f;        // [hdr] Strength: 0 = SDR look, 1 = full highlight extension
  bool hdr_enabled = true;          // [hdr] Enabled: live on/off (also written by the Video menu option)
  // [menu] GameHooks: hook ProcessEvent/CallFunction (needed for the menu option and console automation).
  bool game_hooks = true;
  // [menu] HdrOption: add "HDR: Off/On" to the game's Video options.
  bool menu_hdr_option = true;
  // [debug] ConsoleAtSec: "t:command;t:command" - console commands run on the game thread at t seconds
  // after the first Present (automated tests; bypasses the shipping console's 'say' prefix).
  std::vector<std::pair<int, std::wstring>> console_at_sec;
  // [display] ForceBorderless: turn exclusive fullscreen into a borderless window covering the
  // monitor (required for our own DXGI swapchain in milestone 4).
  bool force_borderless = true;
  // [debug] LogShaders: log the CRC32 of every pixel shader the game creates.
  bool log_shaders = false;
  // [debug] TraceFrameAtSec: log every render-target switch/copy/clear/draw of one frame, N seconds
  // after the first Present (0 = off).
  int trace_frame_at_sec = 0;
  // [debug] CaptureAtSec: comma-separated seconds after the first Present at which to capture the
  // frame (PFM + stats in the log). Requires [output] Mode=dxgi. Empty = off.
  std::vector<int> capture_at_sec;
  // [debug] QuitAfterSec: post WM_CLOSE to the game window N seconds after the first Present (0 = off).
  int quit_after_sec = 0;
  // [debug] MinimizeAtSec / RestoreAtSec: minimize and restore the game window (alt-tab test; 0 = off).
  int minimize_at_sec = 0;
  int restore_at_sec = 0;
  // [debug] RemoveDeviceAtSec: remove the 9on12 D3D12 device N seconds after the first Present (tests
  // OnDeviceRemoved; 0 = off).
  int remove_device_at_sec = 0;
  // [debug] StatsIntervalSec: how often to log FPS and resource counts (0 = off).
  int stats_interval_sec = 0;
};

void Load();
const Settings& Get();
// The HDR menu changes these at runtime (game thread); renderer threads read them every frame.
Settings& Mutable();
// Brightness/strength limits shared by the ini loader and the menu sliders.
constexpr float kMinWhiteNits = 80.0f, kMaxWhiteNits = 500.0f;
constexpr float kMinPeakNits = 400.0f, kMaxPeakNits = 2000.0f;
// Writes one [hdr] key to bl2hdr.ini (value formatted with no decimals unless `decimals` > 0).
void SaveHdr(const wchar_t* key, float value, int decimals = 0);
}  // namespace bl2hdr::config
