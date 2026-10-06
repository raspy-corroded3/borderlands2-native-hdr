#include "proxy.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <mutex>
#include <string>

#include "config.h"
#include "hooks_d3d9.h"
#include "log.h"
#include "process.h"

namespace bl2hdr::proxy {
namespace {
Real g_real;
std::once_flag g_once;

std::wstring ExeDir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring s = path;
  return s.substr(0, s.find_last_of(L'\\') + 1);
}

template <typename T>
void Resolve(T& out, const char* name) {
  out = reinterpret_cast<T>(GetProcAddress(g_real.module, name));
  if (!out) log::Warn("real d3d9: export %s not found", name);
}

// The D3D12 device for 9on12 on the GPU chosen by [d3d9] Gpu. Without one, 9on12 uses the default adapter,
// which on a laptop with two GPUs is usually the integrated one. nullptr = let 9on12 choose.
ID3D12Device* CreateD3D12Device() {
  const std::wstring& gpu = config::Get().gpu;
  DXGI_GPU_PREFERENCE pref;
  if (_wcsicmp(gpu.c_str(), L"high-performance") == 0) {
    pref = DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
  } else if (_wcsicmp(gpu.c_str(), L"minimum-power") == 0) {
    pref = DXGI_GPU_PREFERENCE_MINIMUM_POWER;
  } else {
    if (_wcsicmp(gpu.c_str(), L"system") != 0) log::Warn("[d3d9] Gpu='%ls' unknown - using the system's choice", gpu.c_str());
    return nullptr;
  }
  using CreateFactory_t = HRESULT(WINAPI*)(REFIID, void**);
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  auto create_factory = dxgi ? reinterpret_cast<CreateFactory_t>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
  auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
  IDXGIFactory6* factory = nullptr;
  if (!create_factory || !create_device ||
      FAILED(create_factory(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory)))) {
    log::Warn("GPU selection unavailable (needs Windows 10 1803+) - using the system's choice");
    return nullptr;
  }
  ID3D12Device* dev = nullptr;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; !dev && SUCCEEDED(factory->EnumAdapterByGpuPreference(i, pref, __uuidof(IDXGIAdapter1),
                                                                          reinterpret_cast<void**>(&adapter)));
       ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      const HRESULT hr = create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev));
      log::Info("GPU %u for 9on12 (%ls): '%ls' -> hr=0x%08lX", i, gpu.c_str(), desc.Description,
                static_cast<unsigned long>(hr));
      if (FAILED(hr)) dev = nullptr;
    }
    adapter->Release();
    adapter = nullptr;
  }
  factory->Release();
  if (!dev) log::Warn("no D3D12 device on a hardware GPU - using the system's choice");
  return dev;
}
}  // namespace

bool LoadReal() {
  std::call_once(g_once, [] {
    std::wstring path;
    const auto& chain = config::Get().chain;  // defaults (empty) outside the game: config isn't loaded
    if (!chain.empty() && process::IsGame()) {
      path = ExeDir() + chain;
    } else {
      wchar_t sys[MAX_PATH];
      GetSystemDirectoryW(sys, MAX_PATH);  // WOW64 redirects System32 -> SysWOW64 for this 32-bit process
      path = std::wstring(sys) + L"\\d3d9.dll";
    }
    g_real.module = LoadLibraryW(path.c_str());
    if (!g_real.module) {
      log::Error("failed to load real d3d9 '%ls' (error %lu)", path.c_str(), GetLastError());
      return;
    }
    wchar_t loaded[MAX_PATH];
    GetModuleFileNameW(g_real.module, loaded, MAX_PATH);
    log::Info("real d3d9 loaded: %ls", loaded);
    Resolve(g_real.Direct3DCreate9, "Direct3DCreate9");
    Resolve(g_real.Direct3DCreate9Ex, "Direct3DCreate9Ex");
    Resolve(g_real.D3DPERF_BeginEvent, "D3DPERF_BeginEvent");
    Resolve(g_real.D3DPERF_EndEvent, "D3DPERF_EndEvent");
    Resolve(g_real.D3DPERF_SetMarker, "D3DPERF_SetMarker");
    Resolve(g_real.D3DPERF_SetRegion, "D3DPERF_SetRegion");
    Resolve(g_real.D3DPERF_QueryRepeatFrame, "D3DPERF_QueryRepeatFrame");
    Resolve(g_real.D3DPERF_SetOptions, "D3DPERF_SetOptions");
    Resolve(g_real.D3DPERF_GetStatus, "D3DPERF_GetStatus");
  });
  return g_real.module != nullptr;
}

const Real& Get() { return g_real; }
}  // namespace bl2hdr::proxy

using namespace bl2hdr;

extern "C" {
IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdk_version) {
  if (!proxy::LoadReal() || !proxy::Get().Direct3DCreate9) return nullptr;
  if (!process::IsGame()) return proxy::Get().Direct3DCreate9(sdk_version);  // pass-through
  if (config::Get().Use9On12()) {
    auto create_9on12 = reinterpret_cast<PFN_Direct3DCreate9On12>(
        GetProcAddress(proxy::Get().module, "Direct3DCreate9On12"));
    if (!create_9on12) {
      log::Warn("Mode=9on12 but the real d3d9 has no Direct3DCreate9On12 (chained DLL?) - using native");
    } else {
      D3D9ON12_ARGS args = {};
      args.Enable9On12 = TRUE;
      ID3D12Device* d12 = proxy::CreateD3D12Device();
      args.pD3D12Device = d12;  // nullptr: 9on12 creates its own device on the default adapter
      IDirect3D9* d3d = create_9on12(sdk_version, &args, 1);
      log::Info("Direct3DCreate9On12(sdk=%u, own device %p) -> %p", sdk_version, static_cast<void*>(d12),
                static_cast<void*>(d3d));
      if (d12) d12->Release();  // 9on12 holds its own reference
      if (d3d) {
        hooks::HookDirect3D9(d3d);
        return d3d;
      }
      log::Warn("Direct3DCreate9On12 failed - falling back to native");
    }
  }
  IDirect3D9* d3d = proxy::Get().Direct3DCreate9(sdk_version);
  log::Info("Direct3DCreate9(sdk=%u) -> %p", sdk_version, static_cast<void*>(d3d));
  if (d3d) hooks::HookDirect3D9(d3d);
  return d3d;
}

HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk_version, IDirect3D9Ex** out) {
  if (!proxy::LoadReal() || !proxy::Get().Direct3DCreate9Ex) return D3DERR_NOTAVAILABLE;
  const HRESULT hr = proxy::Get().Direct3DCreate9Ex(sdk_version, out);
  if (!process::IsGame()) return hr;  // pass-through
  log::Info("Direct3DCreate9Ex(sdk=%u) -> hr=0x%08lX", sdk_version, static_cast<unsigned long>(hr));
  if (SUCCEEDED(hr) && out && *out) hooks::HookDirect3D9(*out);
  return hr;
}

int WINAPI Proxy_D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) {
  return proxy::LoadReal() && proxy::Get().D3DPERF_BeginEvent ? proxy::Get().D3DPERF_BeginEvent(c, n) : 0;
}
int WINAPI Proxy_D3DPERF_EndEvent() {
  return proxy::LoadReal() && proxy::Get().D3DPERF_EndEvent ? proxy::Get().D3DPERF_EndEvent() : 0;
}
void WINAPI Proxy_D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) {
  if (proxy::LoadReal() && proxy::Get().D3DPERF_SetMarker) proxy::Get().D3DPERF_SetMarker(c, n);
}
void WINAPI Proxy_D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n) {
  if (proxy::LoadReal() && proxy::Get().D3DPERF_SetRegion) proxy::Get().D3DPERF_SetRegion(c, n);
}
BOOL WINAPI Proxy_D3DPERF_QueryRepeatFrame() {
  return proxy::LoadReal() && proxy::Get().D3DPERF_QueryRepeatFrame ? proxy::Get().D3DPERF_QueryRepeatFrame() : FALSE;
}
void WINAPI Proxy_D3DPERF_SetOptions(DWORD o) {
  if (proxy::LoadReal() && proxy::Get().D3DPERF_SetOptions) proxy::Get().D3DPERF_SetOptions(o);
}
DWORD WINAPI Proxy_D3DPERF_GetStatus() {
  return proxy::LoadReal() && proxy::Get().D3DPERF_GetStatus ? proxy::Get().D3DPERF_GetStatus() : 0;
}
}  // extern "C"
