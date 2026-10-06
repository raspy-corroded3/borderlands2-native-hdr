#include "automation.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "config.h"
#include "log.h"
#include "output_dxgi.h"

namespace bl2hdr::automation {
namespace {
std::atomic<long long> g_first_qpc{0};
size_t g_next_capture = 0;
std::atomic<HWND> g_window{nullptr};
std::once_flag g_thread_once;

double SecondsSinceFirstPresent() {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  return static_cast<double>(now.QuadPart - g_first_qpc.load()) / static_cast<double>(freq.QuadPart);
}

// Window actions run on their own thread: a minimized game stops presenting, so anything driven only
// from Present (restore, quit) would never fire.
void TimerThread() {
  const auto& cfg = config::Get();
  bool minimized = false, restored = false, quit = false;
  while (!quit) {
    Sleep(100);
    const double secs = SecondsSinceFirstPresent();
    HWND hwnd = g_window.load();
    if (!minimized && cfg.minimize_at_sec > 0 && secs >= cfg.minimize_at_sec) {
      minimized = true;
      log::Info("automation: MinimizeAtSec=%d - minimizing %p", cfg.minimize_at_sec, static_cast<void*>(hwnd));
      ShowWindowAsync(hwnd, SW_MINIMIZE);
    }
    if (minimized && !restored && cfg.restore_at_sec > 0 && secs >= cfg.restore_at_sec) {
      restored = true;
      log::Info("automation: RestoreAtSec=%d - restoring %p", cfg.restore_at_sec, static_cast<void*>(hwnd));
      ShowWindowAsync(hwnd, SW_RESTORE);
      SetForegroundWindow(hwnd);
    }
    if (cfg.quit_after_sec > 0 && secs >= cfg.quit_after_sec) {
      quit = true;
      log::Info("automation: QuitAfterSec=%d reached (%.1f s) - posting WM_CLOSE to %p", cfg.quit_after_sec, secs,
                static_cast<void*>(hwnd));
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    if (cfg.quit_after_sec <= 0 && (cfg.minimize_at_sec <= 0 || restored || cfg.restore_at_sec <= 0) &&
        (minimized || cfg.minimize_at_sec <= 0)) {
      break;  // nothing left to do
    }
  }
}
}  // namespace

void OnPresent(IDirect3DDevice9* device) {
  const auto& cfg = config::Get();
  if (cfg.capture_at_sec.empty() && cfg.quit_after_sec <= 0 && cfg.minimize_at_sec <= 0) return;
  if (g_first_qpc.load() == 0) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_first_qpc = now.QuadPart;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    device->GetCreationParameters(&cp);
    g_window = cp.hFocusWindow;
  }
  if (cfg.quit_after_sec > 0 || cfg.minimize_at_sec > 0) {
    std::call_once(g_thread_once, [] { std::thread(TimerThread).detach(); });
  }
  // Captures need a frame, so they stay on the Present path.
  const double secs = SecondsSinceFirstPresent();
  if (g_next_capture < cfg.capture_at_sec.size() && secs >= cfg.capture_at_sec[g_next_capture]) {
    const int at = cfg.capture_at_sec[g_next_capture++];
    output::RequestCapture("t" + std::to_string(at));
  }
}
}  // namespace bl2hdr::automation
