#include "device_removed.h"

#include <d3d12.h>
#include <d3d9on12.h>

#include <atomic>
#include <cwchar>
#include <string>

#include "config.h"
#include "log.h"
#include "output_dxgi.h"

namespace bl2hdr::device_removed {
namespace {
std::atomic<bool> g_reported{false};

// The 9on12 device's D3D12 device (AddRef'd), or nullptr for native D3D9.
template <class T>
T* D3D12DeviceOf(IDirect3DDevice9* device) {
  IDirect3DDevice9On12* on12 = nullptr;
  if (!device || FAILED(device->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&on12))) ||
      !on12) {
    return nullptr;
  }
  T* dev = nullptr;
  if (FAILED(on12->GetD3D12Device(__uuidof(T), reinterpret_cast<void**>(&dev)))) dev = nullptr;
  on12->Release();
  return dev;
}
}  // namespace

void Report(const char* where, HRESULT reason) {
  if (g_reported.exchange(true)) return;
  const std::wstring& action = config::Get().on_device_removed;
  const bool message = _wcsicmp(action.c_str(), L"message") == 0;
  const bool exit = message || _wcsicmp(action.c_str(), L"exit") == 0;
  log::Error("GPU device removed (%s, reason 0x%08lX) - 9on12 cannot recover it; [d3d9] OnDeviceRemoved=%ls: %s", where,
             static_cast<unsigned long>(reason), action.c_str(),
             message ? "message, then the game closes" : exit ? "the game closes" : "the game is left as it is");
  if (!exit) return;

  if (message) {
    // The borderless game window covers the screen: get it out of the way so the message is seen.
    if (HWND hwnd = output::Window()) ShowWindowAsync(hwnd, SW_MINIMIZE);
    wchar_t text[640];
    swprintf_s(text,
               L"Borderlands 2 lost its graphics device: the graphics driver stopped responding or was reset "
               L"(reason 0x%08lX).\n\n"
               L"Direct3D 9on12 cannot recover from this, so the game will close now. Progress since the last "
               L"save point is lost.\n\n"
               L"Common causes: GPU overclock or undervolt, an unstable or just-updated driver, overheating. "
               L"Details are in bl2hdr.log next to Borderlands2.exe.",
               static_cast<unsigned long>(reason));
    MessageBoxW(nullptr, text, L"Borderlands 2 HDR (bl2hdr)", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
  }
  // TerminateProcess, not ExitProcess: the game's threads are stuck on the lost device and may hold locks
  // that DLL_PROCESS_DETACH handlers would wait for.
  log::Info("device removed: ending the process (exit code 0x%08lX)", static_cast<unsigned long>(reason));
  log::Shutdown();
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(reason));
}

void CheckAfterFailure(IDirect3DDevice9* device, const char* where, HRESULT hr) {
  if (g_reported.load() || (hr != D3DERR_DEVICELOST && hr != D3DERR_DEVICEREMOVED && hr != D3DERR_DRIVERINTERNALERROR)) {
    return;
  }
  ID3D12Device* dev = D3D12DeviceOf<ID3D12Device>(device);
  if (!dev) return;
  const HRESULT reason = dev->GetDeviceRemovedReason();
  dev->Release();
  if (FAILED(reason)) Report(where, reason);
}

void DebugRemove(IDirect3DDevice9* device) {
  ID3D12Device5* dev = D3D12DeviceOf<ID3D12Device5>(device);
  if (!dev) {
    log::Warn("debug: RemoveDeviceAtSec - no 9on12 D3D12 device (ID3D12Device5) - skipped");
    return;
  }
  log::Warn("debug: RemoveDeviceAtSec - removing the D3D12 device on purpose");
  dev->RemoveDevice();
  dev->Release();
}
}  // namespace bl2hdr::device_removed
