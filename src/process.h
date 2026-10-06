#pragma once
// Which process loaded the proxy. Set once in DllMain.
namespace bl2hdr::process {
void SetIsGame(bool is_game);
bool IsGame();  // true only inside Borderlands2.exe
}  // namespace bl2hdr::process
