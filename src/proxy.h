#pragma once
// Loads the real d3d9 implementation (system or chained, e.g. DXVK) and resolves its exports.
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <d3d9on12.h>

namespace bl2hdr::proxy {
bool LoadReal();  // idempotent; logs which DLL was loaded

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using Direct3DCreate9Ex_t = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
using D3DPERF_BeginEvent_t = int(WINAPI*)(D3DCOLOR, LPCWSTR);
using D3DPERF_EndEvent_t = int(WINAPI*)();
using D3DPERF_SetMarker_t = void(WINAPI*)(D3DCOLOR, LPCWSTR);
using D3DPERF_SetRegion_t = void(WINAPI*)(D3DCOLOR, LPCWSTR);
using D3DPERF_QueryRepeatFrame_t = BOOL(WINAPI*)();
using D3DPERF_SetOptions_t = void(WINAPI*)(DWORD);
using D3DPERF_GetStatus_t = DWORD(WINAPI*)();

struct Real {
  HMODULE module = nullptr;
  Direct3DCreate9_t Direct3DCreate9 = nullptr;
  Direct3DCreate9Ex_t Direct3DCreate9Ex = nullptr;
  D3DPERF_BeginEvent_t D3DPERF_BeginEvent = nullptr;
  D3DPERF_EndEvent_t D3DPERF_EndEvent = nullptr;
  D3DPERF_SetMarker_t D3DPERF_SetMarker = nullptr;
  D3DPERF_SetRegion_t D3DPERF_SetRegion = nullptr;
  D3DPERF_QueryRepeatFrame_t D3DPERF_QueryRepeatFrame = nullptr;
  D3DPERF_SetOptions_t D3DPERF_SetOptions = nullptr;
  D3DPERF_GetStatus_t D3DPERF_GetStatus = nullptr;
};
const Real& Get();
}  // namespace bl2hdr::proxy
