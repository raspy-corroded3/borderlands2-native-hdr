#pragma once
// Automated testing support: timed frame captures and a clean quit, driven by bl2hdr.ini [debug].
#include <d3d9.h>

namespace bl2hdr::automation {
// Call at every Present (before presenting). Requests captures at [debug] CaptureAtSec times and
// posts WM_CLOSE to the game window at [debug] QuitAfterSec (seconds after the first Present).
void OnPresent(IDirect3DDevice9* device);
}  // namespace bl2hdr::automation
