#pragma once
// Milestone 5: "HDR: Off / On" in the game's Video options, plus game-thread console commands for
// automated tests. Built on ue3.h (ProcessEvent / CallFunction hooks).
namespace bl2hdr::menu {
// Called once on the first Present (engine initialised): resolves the script functions and enables
// the hooks. Requires ue3::Init() to have succeeded (done in DllMain).
void OnFirstPresent();
// Live HDR state (Video menu / [hdr] Enabled). Off = strength 0 (SDR look in the HDR container).
bool HdrEnabled();
// Seconds since the first Present (shared time base with automation; 0 before it).
void SetTimeBase(long long first_present_qpc);
}  // namespace bl2hdr::menu
