#pragma once
// Window/display-mode handling (milestone 4a).
#include <windows.h>
#include <d3d9.h>

namespace bl2hdr::window {
// If [display] ForceBorderless is on and the game asks for exclusive fullscreen, rewrite the
// present parameters to windowed and make the window a borderless popup covering its monitor.
// Call before the real CreateDevice/Reset. `focus` is the device focus window (fallback HWND).
void ApplyBorderless(D3DPRESENT_PARAMETERS* pp, HWND focus, const char* where);
}  // namespace bl2hdr::window
