#pragma once
// What happens when the GPU device is removed (driver crash/reset, TDR). D3D9on12 cannot recover a
// removed D3D12 device: the game's Present/Reset return D3DERR_DEVICELOST forever and the game hangs.
// [d3d9] OnDeviceRemoved: "message" (default: tell the player, then end the process), "exit" (end the
// process without a message) or "continue" (only log; the game stays as it is).
#include <windows.h>
#include <d3d9.h>

namespace bl2hdr::device_removed {
// The D3D12 device is gone: `where` raised the suspicion, `reason` is GetDeviceRemovedReason. Acts once.
void Report(const char* where, HRESULT reason);
// After a failed Present/Reset of the game: checks whether the 9on12 device's D3D12 device was removed
// (no-op for native D3D9, where the game's own device-lost handling works).
void CheckAfterFailure(IDirect3DDevice9* device, const char* where, HRESULT hr);
// [debug] RemoveDeviceAtSec: removes the 9on12 D3D12 device on purpose (tests this path).
void DebugRemove(IDirect3DDevice9* device);
}  // namespace bl2hdr::device_removed
