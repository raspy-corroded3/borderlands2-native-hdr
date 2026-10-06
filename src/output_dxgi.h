#pragma once
// Milestone 4c: present the game's frames through our own DXGI swapchain on the D3D12 device that
// D3D9on12 runs on, instead of D3D9's Present (which does not reach the screen when windowed under
// 9on12 on the test system).
#include <windows.h>
#include <d3d9.h>

#include <string>

namespace bl2hdr::output {
// Remember the window the device renders to (called after CreateDevice/Reset).
void SetWindow(HWND hwnd, UINT width, UINT height);
// True when [output] Mode=dxgi and the device is 9on12.
bool Enabled(IDirect3DDevice9* device);
// Replaces IDirect3DDevice9::Present. Returns the HRESULT to give back to the game.
HRESULT Present(IDirect3DDevice9* device);
// Release swapchain/D3D12 objects before the device is reset or destroyed.
void OnReset();
// Capture the next presented source frame (FP16 substitute or 8-bit back buffer) to
// <exe dir>\bl2hdr_captures\<label>.pfm and log HDR statistics. Only works on the dxgi output path.
void RequestCapture(const std::string& label);
// Local (dedicated) video memory used by this process and the OS budget for it, in bytes.
// False until the dxgi output is running.
bool VideoMemory(unsigned long long* usage, unsigned long long* budget);
}  // namespace bl2hdr::output
