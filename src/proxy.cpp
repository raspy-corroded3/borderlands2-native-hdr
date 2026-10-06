#include "proxy.h"

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
      args.Enable9On12 = TRUE;  // let 9on12 create its own D3D12 device on the default adapter
      IDirect3D9* d3d = create_9on12(sdk_version, &args, 1);
      log::Info("Direct3DCreate9On12(sdk=%u) -> %p", sdk_version, static_cast<void*>(d3d));
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
