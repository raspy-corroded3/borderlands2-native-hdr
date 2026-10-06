#include "window.h"

#include "config.h"
#include "log.h"

namespace bl2hdr::window {

void ApplyBorderless(D3DPRESENT_PARAMETERS* pp, HWND focus, const char* where) {
  if (!pp || !config::Get().force_borderless) return;
  HWND hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
  if (!hwnd) {
    log::Warn("%s: ForceBorderless: no window handle", where);
    return;
  }
  // Only an exclusive-fullscreen request is converted. A windowed request (BL2's borderless mode, or a
  // bordered window the user chose) is left to the game: restyling it makes the game resize and Reset
  // again (e.g. setres 1920x1080w, then forced to 2560x1440, made the game Reset to 2416x1360).
  const bool was_fullscreen = !pp->Windowed;
  if (!was_fullscreen) return;
  pp->Windowed = TRUE;
  pp->FullScreen_RefreshRateInHz = 0;  // must be 0 when windowed

  // Borderless popup covering the monitor the window is on.
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
  const RECT& r = mi.rcMonitor;
  LONG style = GetWindowLongW(hwnd, GWL_STYLE);
  LONG new_style = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER |
                              WS_DLGFRAME)) |
                   WS_POPUP | WS_VISIBLE;
  SetWindowLongW(hwnd, GWL_STYLE, new_style);
  LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
  SetWindowLongW(hwnd, GWL_EXSTYLE, ex & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
  SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
               SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);

  // Keep the back buffer at the monitor size if the game left it at 0 (= use window size).
  if (pp->BackBufferWidth == 0) pp->BackBufferWidth = static_cast<UINT>(r.right - r.left);
  if (pp->BackBufferHeight == 0) pp->BackBufferHeight = static_cast<UINT>(r.bottom - r.top);

  log::Info("%s: ForceBorderless: %s -> windowed borderless %ldx%ld at (%ld,%ld), style 0x%08lX -> 0x%08lX, back buffer %ux%u",
            where, was_fullscreen ? "exclusive fullscreen" : "windowed", r.right - r.left, r.bottom - r.top, r.left,
            r.top, style, new_style, pp->BackBufferWidth, pp->BackBufferHeight);
}

}  // namespace bl2hdr::window
