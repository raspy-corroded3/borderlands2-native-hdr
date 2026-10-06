// Milestone 1: observation-only hooks. Nothing here changes what the game renders.
// Vtable patching: each hooked slot remembers the original function per vtable, so several
// implementations (e.g. system d3d9 and DXVK, or 9 and 9Ex classes) are handled correctly.
#include "hooks_d3d9.h"

#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <d3d9on12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "config.h"
#include "log.h"
#include "shader_hash.h"
#include "shader_swap.h"
#include "automation.h"
#include "backbuffer.h"
#include "menu.h"
#include "output_dxgi.h"
#include "vtable_index.h"
#include "window.h"

namespace bl2hdr::hooks {
namespace {

// ---------- vtable patching ----------
struct Slot {
  std::mutex mutex;
  using Map = std::unordered_map<void**, void*>;
  Map originals;  // vtable -> original function; entries are never erased or changed

  // Lock-free fast path for the common case of a single implementation (hot hooks run per draw):
  // the last map entry used. One pointer, so vtable and function are always read as a pair; map
  // nodes do not move on rehash and are never erased, so the pointer stays valid.
  std::atomic<const Map::value_type*> last{nullptr};

  template <typename F>
  F Original(void* self) {
    void** vtbl = *static_cast<void***>(self);
    const Map::value_type* entry = last.load(std::memory_order_acquire);
    if (entry && entry->first == vtbl) return reinterpret_cast<F>(entry->second);
    std::lock_guard lock(mutex);
    auto it = originals.find(vtbl);
    if (it == originals.end()) return nullptr;
    last.store(&*it, std::memory_order_release);
    return reinterpret_cast<F>(it->second);
  }

  void Patch(void* obj, unsigned index, void* hook, const char* name) {
    void** vtbl = *static_cast<void***>(obj);
    std::lock_guard lock(mutex);
    if (originals.contains(vtbl)) return;
    if (vtbl[index] == hook) return;
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
      log::Error("hook %s: VirtualProtect failed (%lu)", name, GetLastError());
      return;
    }
    originals[vtbl] = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[index], sizeof(void*));
    log::Info("hooked %s (vtable %p slot %u, original %p)", name, static_cast<void*>(vtbl), index,
              originals[vtbl]);
  }
};

Slot s_create_device, s_reset, s_present, s_create_texture, s_create_volume, s_create_cube,
    s_create_vb, s_create_ib, s_create_rt, s_create_ps;
// Frame-trace hooks (installed only when [debug] TraceFrameAtSec > 0).
Slot s_get_bb, s_stretch, s_set_rt, s_clear, s_set_ps, s_draw, s_draw_idx, s_draw_up, s_draw_idx_up, s_set_tex;

// ---------- formatting helpers ----------
std::string FormatName(D3DFORMAT f) {
  const auto v = static_cast<unsigned>(f);
  if (v > 0xFF) {  // FourCC formats (DXT1, INTZ, NULL, ...)
    char s[5] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                 static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF), 0};
    return s;
  }
  switch (f) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_A2R10G10B10: return "A2R10G10B10";
    case D3DFMT_A2B10G10R10: return "A2B10G10R10";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_A16B16G16R16: return "A16B16G16R16";
    case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
    case D3DFMT_R16F: return "R16F";
    case D3DFMT_R32F: return "R32F";
    case D3DFMT_G16R16F: return "G16R16F";
    case D3DFMT_G16R16: return "G16R16";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_D16: return "D16";
    case D3DFMT_L8: return "L8";
    case D3DFMT_A8: return "A8";
    case D3DFMT_A8L8: return "A8L8";
    case D3DFMT_V8U8: return "V8U8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_UNKNOWN: return "UNKNOWN";
    default: return "fmt" + std::to_string(v);
  }
}
const char* PoolName(D3DPOOL p) {
  switch (p) {
    case D3DPOOL_DEFAULT: return "DEFAULT";
    case D3DPOOL_MANAGED: return "MANAGED";
    case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
    case D3DPOOL_SCRATCH: return "SCRATCH";
    default: return "?";
  }
}
void LogPresentParams(const char* what, const D3DPRESENT_PARAMETERS* pp) {
  if (!pp) return;
  log::Info("%s: %ux%u fmt=%s count=%u msaa=%d/%lu swap=%d windowed=%d autodepth=%d(%s) flags=0x%lX refresh=%u interval=0x%X hwnd=%p",
            what, pp->BackBufferWidth, pp->BackBufferHeight, FormatName(pp->BackBufferFormat).c_str(),
            pp->BackBufferCount, pp->MultiSampleType, pp->MultiSampleQuality, pp->SwapEffect, pp->Windowed,
            pp->EnableAutoDepthStencil, FormatName(pp->AutoDepthStencilFormat).c_str(), pp->Flags,
            pp->FullScreen_RefreshRateInHz, pp->PresentationInterval, static_cast<void*>(pp->hDeviceWindow));
}

// ---------- statistics ----------
enum Kind { kTex, kVol, kCube, kVB, kIB, kRT, kKinds };
const char* const kKindNames[kKinds] = {"tex", "vol", "cube", "vb", "ib", "rtsurf"};
std::atomic<unsigned> g_counts[kKinds][4];  // [kind][pool]
std::atomic<unsigned long long> g_frames{0};
std::atomic<unsigned long long> g_frames_at_last_stats{0};
std::atomic<long long> g_last_stats_qpc{0};
std::mutex g_shader_mutex;
std::unordered_set<uint32_t> g_seen_shaders;
std::atomic<unsigned> g_ps_created{0};

void Count(Kind k, D3DPOOL pool) {
  if (static_cast<unsigned>(pool) < 4) g_counts[k][pool].fetch_add(1, std::memory_order_relaxed);
}

void LogMemory();

void MaybeLogStats() {
  const int interval = config::Get().stats_interval_sec;
  if (interval <= 0) return;
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  long long last = g_last_stats_qpc.load();
  if (last == 0) { g_last_stats_qpc = now.QuadPart; g_frames_at_last_stats = g_frames.load(); return; }
  const double secs = static_cast<double>(now.QuadPart - last) / static_cast<double>(freq.QuadPart);
  if (secs < interval) return;
  if (!g_last_stats_qpc.compare_exchange_strong(last, now.QuadPart)) return;
  const unsigned long long frames = g_frames.load();
  const double fps = static_cast<double>(frames - g_frames_at_last_stats.exchange(frames)) / secs;
  std::string line;
  for (int k = 0; k < kKinds; ++k) {
    line += std::string(" ") + kKindNames[k] + "[D" + std::to_string(g_counts[k][0].load()) + " M" +
            std::to_string(g_counts[k][1].load()) + " S" + std::to_string(g_counts[k][2].load()) + " X" +
            std::to_string(g_counts[k][3].load()) + "]";
  }
  log::Info("stats: %.1f fps, frame %llu, created (D=DEFAULT M=MANAGED S=SYSTEMMEM X=SCRATCH):%s ps=%u unique_ps=%zu",
            fps, frames, line.c_str(), g_ps_created.load(), g_seen_shaders.size());
  LogMemory();
}

// Process memory, to tell address-space exhaustion (32-bit game) from running out of video memory:
// free address space and its largest free block, committed private bytes, local video memory vs budget,
// and what the used address space holds (DLL images, mapped views, private; reserved-only counted apart).
void LogMemory() {
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  GlobalMemoryStatusEx(&ms);
  unsigned long long largest = 0, image = 0, mapped = 0, priv = 0, reserved = 0;
  MEMORY_BASIC_INFORMATION mbi{};
  for (const char* p = nullptr; VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi);) {
    if (mbi.State == MEM_FREE && mbi.RegionSize > largest) largest = mbi.RegionSize;
    if (mbi.State == MEM_RESERVE) {
      reserved += mbi.RegionSize;
    } else if (mbi.State == MEM_COMMIT) {
      (mbi.Type == MEM_IMAGE ? image : mbi.Type == MEM_MAPPED ? mapped : priv) += mbi.RegionSize;
    }
    const char* next = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
    if (next <= p) break;
    p = next;
  }
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  constexpr double kMiB = 1024.0 * 1024.0;
  unsigned long long vram = 0, budget = 0;
  char vram_text[64] = "n/a";
  if (output::VideoMemory(&vram, &budget)) {
    std::snprintf(vram_text, sizeof(vram_text), "%.0f / %.0f MiB", vram / kMiB, budget / kMiB);
  }
  log::Info("memory: address space free %.0f of %.0f MiB (largest block %.0f MiB), private %.0f MiB, video %s",
            ms.ullAvailVirtual / kMiB, ms.ullTotalVirtual / kMiB, largest / kMiB, pmc.PrivateUsage / kMiB, vram_text);
  log::Info("memory: committed address space: images %.0f MiB, mapped %.0f MiB, private %.0f MiB; reserved %.0f MiB",
            image / kMiB, mapped / kMiB, priv / kMiB, reserved / kMiB);
}

// ---------- one-frame trace (milestone 4 HDR stage) ----------
// Logs the sequence of render-target switches, copies, clears and draws of one whole frame (between
// two Presents), so we can see where the tonemap pass writes and how the image reaches the back buffer.
std::mutex g_ps_map_mutex;
std::unordered_map<IDirect3DPixelShader9*, uint32_t> g_ps_crc;  // shader object -> bytecode CRC

// Only frame traces read the map. It holds raw pointers of shaders that may be released later (an
// address can be reused; the newest entry wins), so it is filled only when a trace is configured.
void RememberCrc(IDirect3DPixelShader9* ps, uint32_t crc) {
  if (config::Get().trace_frame_at_sec <= 0) return;
  std::lock_guard lock(g_ps_map_mutex);
  g_ps_crc[ps] = crc;  // original CRC, also for swapped shaders, so traces show which pass it is
}

struct Trace {
  enum class State { kIdle, kArmed, kTracing, kDone } state = State::kIdle;
  long long first_present_qpc = 0;
  IDirect3DSurface9* backbuffer = nullptr;  // raw pointer, only compared
  IDirect3DPixelShader9* current_ps = nullptr;
  unsigned draws = 0;                       // draws in the current segment
  std::vector<uint32_t> segment_ps;         // distinct pixel shaders used in the segment
  unsigned total_draws = 0, events = 0;
} g_trace;

uint32_t PsCrc(IDirect3DPixelShader9* ps) {
  if (!ps) return 0;
  std::lock_guard lock(g_ps_map_mutex);
  auto it = g_ps_crc.find(ps);
  return it == g_ps_crc.end() ? 0xFFFFFFFFu : it->second;
}

std::string DescribeSurface(IDirect3DSurface9* s) {
  if (!s) return "null";
  D3DSURFACE_DESC d{};
  s->GetDesc(&d);
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%p %ux%u %s", static_cast<void*>(s), d.Width, d.Height, FormatName(d.Format).c_str());
  std::string out = buf;
  if (s == g_trace.backbuffer) out += " [BACKBUFFER]";
  IDirect3DTexture9* tex = nullptr;
  if (SUCCEEDED(s->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) && tex) {
    std::snprintf(buf, sizeof(buf), " (texture %p)", static_cast<void*>(tex));
    out += buf;
    tex->Release();
  }
  return out;
}

bool Tracing() { return g_trace.state == Trace::State::kTracing; }

void FlushSegment() {
  if (!Tracing() || g_trace.draws == 0) return;
  std::string shaders;
  for (uint32_t c : g_trace.segment_ps) {
    char b[48];
    const char* known = shader::KnownName(c);
    std::snprintf(b, sizeof(b), " %08X%s", c, known ? "*" : "");
    shaders += b;
  }
  log::Info("TRACE   %u draw(s), pixel shaders:%s", g_trace.draws, shaders.c_str());
  g_trace.total_draws += g_trace.draws;
  g_trace.draws = 0;
  g_trace.segment_ps.clear();
}

void TraceEvent(const char* fmt, ...) {
  if (!Tracing()) return;
  FlushSegment();
  char buf[512];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, a);
  va_end(a);
  log::Info("TRACE %s", buf);
  ++g_trace.events;
}

void TraceDraw() {
  if (!Tracing()) return;
  ++g_trace.draws;
  const uint32_t c = PsCrc(g_trace.current_ps);
  if (std::find(g_trace.segment_ps.begin(), g_trace.segment_ps.end(), c) == g_trace.segment_ps.end() &&
      g_trace.segment_ps.size() < 8) {
    g_trace.segment_ps.push_back(c);
  }
}

// Called at the start of every Present: ends a running trace, or arms one when it is time.
void TraceOnPresent(IDirect3DDevice9* dev) {
  const int at = config::Get().trace_frame_at_sec;
  if (at <= 0 || g_trace.state == Trace::State::kDone) return;
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  if (g_trace.first_present_qpc == 0) g_trace.first_present_qpc = now.QuadPart;
  if (g_trace.state == Trace::State::kTracing) {
    FlushSegment();
    log::Info("TRACE ==== end of frame: %u draws, %u events ====", g_trace.total_draws, g_trace.events);
    g_trace.state = Trace::State::kDone;
    return;
  }
  const double secs = static_cast<double>(now.QuadPart - g_trace.first_present_qpc) / static_cast<double>(freq.QuadPart);
  if (g_trace.state == Trace::State::kIdle && secs >= at) {
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
      g_trace.backbuffer = bb;
      bb->Release();
    }
    IDirect3DSurface9* rt0 = nullptr;
    std::string rt0_desc = "?";
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt0)) && rt0) {
      rt0_desc = DescribeSurface(rt0);
      rt0->Release();
    }
    g_trace.state = Trace::State::kTracing;
    log::Info("TRACE ==== frame trace starts (%.1f s after first Present) ==== back buffer %s; RT0 at start %s", secs,
              DescribeSurface(g_trace.backbuffer).c_str(), rt0_desc.c_str());
  }
}

HRESULT STDMETHODCALLTYPE GetBackBuffer_Hook(IDirect3DDevice9* self, UINT sc, UINT idx, D3DBACKBUFFER_TYPE type,
                                             IDirect3DSurface9** out) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9**);
  static std::atomic<int> logged{0};
  // HDR stage: hand the game our FP16 substitute instead of the 8-bit back buffer.
  if (IDirect3DSurface9* sub = backbuffer::Surface(); sub && sc == 0 && idx == 0 && out) {
    sub->AddRef();
    *out = sub;
    if (logged.fetch_add(1) < 5) log::Info("GetBackBuffer(0,0) -> FP16 substitute %p", static_cast<void*>(sub));
    return D3D_OK;
  }
  const HRESULT hr = s_get_bb.Original<Fn>(self)(self, sc, idx, type, out);
  if (logged.fetch_add(1) < 20) {
    log::Info("GetBackBuffer(swapchain %u, index %u) -> hr=0x%08lX %p", sc, idx, static_cast<unsigned long>(hr),
              out ? static_cast<void*>(*out) : nullptr);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE StretchRect_Hook(IDirect3DDevice9* self, IDirect3DSurface9* src, const RECT* sr,
                                           IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE filter) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*,
                                         const RECT*, D3DTEXTUREFILTERTYPE);
  if (Tracing()) {
    TraceEvent("StretchRect %s -> %s filter=%d", DescribeSurface(src).c_str(), DescribeSurface(dst).c_str(), filter);
  }
  return s_stretch.Original<Fn>(self)(self, src, sr, dst, dr, filter);
}

HRESULT STDMETHODCALLTYPE SetRenderTarget_Hook(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* rt) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
  if (Tracing()) TraceEvent("SetRenderTarget[%lu] = %s", index, DescribeSurface(rt).c_str());
  return s_set_rt.Original<Fn>(self)(self, index, rt);
}

HRESULT STDMETHODCALLTYPE Clear_Hook(IDirect3DDevice9* self, DWORD count, const D3DRECT* rects, DWORD flags,
                                     D3DCOLOR color, float z, DWORD stencil) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
  if (Tracing()) TraceEvent("Clear flags=0x%lX color=0x%08lX rects=%lu", flags, color, count);
  return s_clear.Original<Fn>(self)(self, count, rects, flags, color, z, stencil);
}

// Our HDR tonemap shader object (set when the replacement is created); its parameters live in c50.
std::atomic<IDirect3DPixelShader9*> g_hdr_tonemap_ps{nullptr};

HRESULT STDMETHODCALLTYPE SetPixelShader_Hook(IDirect3DDevice9* self, IDirect3DPixelShader9* ps) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
  if (Tracing()) {
    g_trace.current_ps = ps;
    const uint32_t c = PsCrc(ps);
    if (const char* known = shader::KnownName(c)) TraceEvent("SetPixelShader %08X = %s", c, known);
  }
  const HRESULT hr = s_set_ps.Original<Fn>(self)(self, ps);
  if (ps && ps == g_hdr_tonemap_ps.load(std::memory_order_relaxed)) {
    const auto& cfg = config::Get();
    // Without the HDR output the back buffer is 8-bit: keep strength 0 and no game/UI scaling.
    const bool hdr_out = backbuffer::Surface() != nullptr;
    const float strength = hdr_out && menu::HdrEnabled() ? cfg.hdr_strength : 0.0f;  // live Off = 0 (menu option)
    const float game_to_ui = hdr_out ? cfg.paper_white_nits / cfg.ui_white_nits : 1.0f;
    const float params[4] = {cfg.peak_nits / cfg.paper_white_nits, strength, game_to_ui, 0.0f};
    self->SetPixelShaderConstantF(50, params, 1);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE SetTexture_Hook(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* tex) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
  if (Tracing() && shader::KnownName(PsCrc(g_trace.current_ps)) && stage < 8) {
    std::string d = "null";
    IDirect3DTexture9* t2 = nullptr;
    if (tex && SUCCEEDED(tex->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&t2))) && t2) {
      D3DSURFACE_DESC sd{};
      t2->GetLevelDesc(0, &sd);
      char b[96];
      std::snprintf(b, sizeof(b), "%p %ux%u %s", static_cast<void*>(tex), sd.Width, sd.Height, FormatName(sd.Format).c_str());
      d = b;
      t2->Release();
    }
    TraceEvent("  SetTexture s%lu = %s", stage, d.c_str());
  }
  return s_set_tex.Original<Fn>(self)(self, stage, tex);
}

HRESULT STDMETHODCALLTYPE DrawPrimitive_Hook(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT start, UINT count) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
  TraceDraw();
  return s_draw.Original<Fn>(self)(self, t, start, count);
}
HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive_Hook(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, INT base, UINT min_idx,
                                                    UINT num_v, UINT start, UINT count) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
  TraceDraw();
  return s_draw_idx.Original<Fn>(self)(self, t, base, min_idx, num_v, start, count);
}
HRESULT STDMETHODCALLTYPE DrawPrimitiveUP_Hook(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT count, const void* data,
                                               UINT stride) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
  TraceDraw();
  return s_draw_up.Original<Fn>(self)(self, t, count, data, stride);
}
HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP_Hook(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT min_idx,
                                                      UINT num_v, UINT count, const void* idx, D3DFORMAT fmt,
                                                      const void* data, UINT stride) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                         const void*, UINT);
  TraceDraw();
  return s_draw_idx_up.Original<Fn>(self)(self, t, min_idx, num_v, count, idx, fmt, data, stride);
}

// ---------- D3D9on12 probe (milestone 2 decision) ----------
// Logs whether the device exposes IDirect3DDevice9On12 and which D3D12 device/GPU sits underneath.
void Probe9On12(IDirect3DDevice9* dev) {
  IDirect3DDevice9On12* on12 = nullptr;
  HRESULT hr = dev->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&on12));
  if (FAILED(hr) || !on12) {
    log::Info("9on12 probe: device is NOT 9on12 (QueryInterface hr=0x%08lX)", static_cast<unsigned long>(hr));
    return;
  }
  ID3D12Device* d12 = nullptr;
  hr = on12->GetD3D12Device(__uuidof(ID3D12Device), reinterpret_cast<void**>(&d12));
  if (SUCCEEDED(hr) && d12) {
    const LUID luid = d12->GetAdapterLuid();
    std::wstring adapter = L"?";
    using CreateFactory_t = HRESULT(WINAPI*)(REFIID, void**);
    if (HMODULE dxgi = LoadLibraryW(L"dxgi.dll")) {
      auto create = reinterpret_cast<CreateFactory_t>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
      IDXGIFactory4* factory = nullptr;
      if (create && SUCCEEDED(create(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) && factory) {
        IDXGIAdapter1* a = nullptr;
        if (SUCCEEDED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&a))) && a) {
          DXGI_ADAPTER_DESC1 desc{};
          a->GetDesc1(&desc);
          adapter = desc.Description;
          a->Release();
        }
        factory->Release();
      }
    }
    log::Info("9on12 probe: OK - IDirect3DDevice9On12 available, D3D12 device %p, nodes=%u, adapter '%ls'",
              static_cast<void*>(d12), d12->GetNodeCount(), adapter.c_str());
    d12->Release();
  } else {
    log::Warn("9on12 probe: GetD3D12Device failed hr=0x%08lX", static_cast<unsigned long>(hr));
  }
  on12->Release();
}

// ---------- IDirect3D9 ----------
HRESULT STDMETHODCALLTYPE CreateDevice_Hook(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                            DWORD behavior, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                         IDirect3DDevice9**);
  auto orig = s_create_device.Original<Fn>(self);
  log::Info("CreateDevice: adapter=%u type=%d focus=%p behavior=0x%08lX", adapter, type, static_cast<void*>(focus),
            behavior);
  LogPresentParams("CreateDevice params", pp);
  if (config::Get().force_borderless) {
    window::ApplyBorderless(pp, focus, "CreateDevice");
    LogPresentParams("CreateDevice params (after ForceBorderless)", pp);
  }
  const HRESULT hr = orig(self, adapter, type, focus, behavior, pp, out);
  log::Info("CreateDevice -> hr=0x%08lX device=%p", static_cast<unsigned long>(hr),
            out ? static_cast<void*>(*out) : nullptr);
  if (SUCCEEDED(hr) && out && *out) {
    output::SetWindow(pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus, pp ? pp->BackBufferWidth : 0,
                      pp ? pp->BackBufferHeight : 0);
    HookDevice(*out);
    if (config::Get().Use9On12()) Probe9On12(*out);
    if (pp) backbuffer::Create(*out, pp->BackBufferWidth, pp->BackBufferHeight);
    if (IDirect3DSurface9* sub = backbuffer::Surface()) (*out)->SetRenderTarget(0, sub);
  }
  return hr;
}

// ---------- IDirect3DDevice9 ----------
HRESULT STDMETHODCALLTYPE Reset_Hook(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
  LogPresentParams("Reset", pp);
  if (config::Get().force_borderless) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    self->GetCreationParameters(&cp);
    window::ApplyBorderless(pp, cp.hFocusWindow, "Reset");
    LogPresentParams("Reset (after ForceBorderless)", pp);
  }
  output::OnReset();      // our swapchain must go before the device's resources are reset
  backbuffer::Release();  // D3DPOOL_DEFAULT render targets must be released before Reset
  const HRESULT hr = s_reset.Original<Fn>(self)(self, pp);
  log::Info("Reset -> hr=0x%08lX", static_cast<unsigned long>(hr));
  if (SUCCEEDED(hr) && pp) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    self->GetCreationParameters(&cp);
    output::SetWindow(pp->hDeviceWindow ? pp->hDeviceWindow : cp.hFocusWindow, pp->BackBufferWidth,
                      pp->BackBufferHeight);
    backbuffer::Create(self, pp->BackBufferWidth, pp->BackBufferHeight);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE Present_Hook(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND wnd,
                                       const RGNDATA* dirty) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
  TraceOnPresent(self);
  static std::once_flag first_present;
  std::call_once(first_present, [] {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    menu::SetTimeBase(now.QuadPart);
    menu::OnFirstPresent();
  });
  automation::OnPresent(self);
  HRESULT hr = E_FAIL;
  bool presented = false;
  if (output::Enabled(self)) {  // milestone 4c: our own DXGI swapchain instead of D3D9's Present
    hr = output::Present(self);
    presented = SUCCEEDED(hr);
  }
  if (!presented) hr = s_present.Original<Fn>(self)(self, src, dst, wnd, dirty);
  // The device's implicit render target is the real back buffer; point it at the substitute so
  // anything drawn before the game's first SetRenderTarget of the frame also lands in FP16.
  if (presented) {
    if (IDirect3DSurface9* sub = backbuffer::Surface()) self->SetRenderTarget(0, sub);
  }
  if (g_frames.fetch_add(1) == 0) log::Info("first Present -> hr=0x%08lX", static_cast<unsigned long>(hr));
  if (FAILED(hr)) log::Warn("Present -> hr=0x%08lX", static_cast<unsigned long>(hr));
  MaybeLogStats();
  return hr;
}

HRESULT STDMETHODCALLTYPE CreateTexture_Hook(IDirect3DDevice9* self, UINT w, UINT h, UINT levels, DWORD usage,
                                             D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DTexture9**, HANDLE*);
  const HRESULT hr = s_create_texture.Original<Fn>(self)(self, w, h, levels, usage, fmt, pool, out, shared);
  Count(kTex, pool);
  if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
    log::Info("CreateTexture %s %ux%u levels=%u usage=0x%lX fmt=%s pool=%s -> hr=0x%08lX",
              (usage & D3DUSAGE_RENDERTARGET) ? "RT" : "DS", w, h, levels, usage, FormatName(fmt).c_str(),
              PoolName(pool), static_cast<unsigned long>(hr));
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE CreateVolumeTexture_Hook(IDirect3DDevice9* self, UINT w, UINT h, UINT d, UINT levels,
                                                   DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                   IDirect3DVolumeTexture9** out, HANDLE* shared) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DVolumeTexture9**, HANDLE*);
  Count(kVol, pool);
  return s_create_volume.Original<Fn>(self)(self, w, h, d, levels, usage, fmt, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE CreateCubeTexture_Hook(IDirect3DDevice9* self, UINT edge, UINT levels, DWORD usage,
                                                 D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9** out,
                                                 HANDLE* shared) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DCubeTexture9**, HANDLE*);
  Count(kCube, pool);
  if (usage & D3DUSAGE_RENDERTARGET) {
    log::Info("CreateCubeTexture RT edge=%u fmt=%s pool=%s", edge, FormatName(fmt).c_str(), PoolName(pool));
  }
  return s_create_cube.Original<Fn>(self)(self, edge, levels, usage, fmt, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE CreateVertexBuffer_Hook(IDirect3DDevice9* self, UINT len, DWORD usage, DWORD fvf,
                                                  D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**,
                                         HANDLE*);
  Count(kVB, pool);
  return s_create_vb.Original<Fn>(self)(self, len, usage, fvf, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE CreateIndexBuffer_Hook(IDirect3DDevice9* self, UINT len, DWORD usage, D3DFORMAT fmt,
                                                 D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DIndexBuffer9**, HANDLE*);
  Count(kIB, pool);
  return s_create_ib.Original<Fn>(self)(self, len, usage, fmt, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE CreateRenderTarget_Hook(IDirect3DDevice9* self, UINT w, UINT h, D3DFORMAT fmt,
                                                  D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL lockable,
                                                  IDirect3DSurface9** out, HANDLE* shared) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                         IDirect3DSurface9**, HANDLE*);
  const HRESULT hr = s_create_rt.Original<Fn>(self)(self, w, h, fmt, ms, msq, lockable, out, shared);
  Count(kRT, D3DPOOL_DEFAULT);
  log::Info("CreateRenderTarget %ux%u fmt=%s msaa=%d lockable=%d -> hr=0x%08lX", w, h, FormatName(fmt).c_str(), ms,
            lockable, static_cast<unsigned long>(hr));
  return hr;
}

HRESULT STDMETHODCALLTYPE CreatePixelShader_Hook(IDirect3DDevice9* self, const DWORD* func,
                                                 IDirect3DPixelShader9** out) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
  auto orig = s_create_ps.Original<Fn>(self);
  g_ps_created.fetch_add(1, std::memory_order_relaxed);
  if (!func) return orig(self, func, out);

  const size_t size = shader::BytecodeSize(reinterpret_cast<const uint32_t*>(func));
  const uint32_t crc = size ? shader::Crc32(func, size) : 0;
  bool first = false;
  {
    std::lock_guard lock(g_shader_mutex);
    first = g_seen_shaders.insert(crc).second;
  }
  const char* known = shader::KnownName(crc);
  if (config::Get().log_shaders) {
    if (known) {
      log::Info("CreatePixelShader 0x%08X size=%zu  <-- TARGET: %s", crc, size, known);
    } else if (first) {
      log::Info("CreatePixelShader 0x%08X size=%zu", crc, size);
    }
  }

  // Milestone 3: swap known shaders for our compiled replacements (see shader_swap.cpp).
  const shader_swap::Replacement rep = shader_swap::For(crc);
  if (rep.bytecode) {
    const HRESULT hr = orig(self, static_cast<const DWORD*>(rep.bytecode), out);
    if (SUCCEEDED(hr)) {
      log::Info("CreatePixelShader 0x%08X: replaced with '%s' (%zu bytes)", crc, rep.variant, rep.size);
      if (out && *out) RememberCrc(*out, crc);
      if (out && *out && config::Get().tonemap == config::TonemapVariant::kHdr && crc == 0x54ED86A0u) {
        // Keep a reference: while we hold it, no other shader can be created at this address and be
        // mistaken for the tonemapper (it would get our c50 parameters).
        (*out)->AddRef();
        if (IDirect3DPixelShader9* old = g_hdr_tonemap_ps.exchange(*out)) old->Release();
      }
      return hr;
    }
    log::Error("CreatePixelShader 0x%08X: replacement '%s' failed hr=0x%08lX - using the original", crc,
               rep.variant, static_cast<unsigned long>(hr));
  }
  const HRESULT hr = orig(self, func, out);
  if (SUCCEEDED(hr) && out && *out) RememberCrc(*out, crc);
  return hr;
}

}  // namespace

void HookDirect3D9(IDirect3D9* d3d) {
  s_create_device.Patch(d3d, kSlotD3D9_CreateDevice, reinterpret_cast<void*>(&CreateDevice_Hook),
                        "IDirect3D9::CreateDevice");
}

void HookDevice(IDirect3DDevice9* dev) {
  s_reset.Patch(dev, kSlotDev_Reset, reinterpret_cast<void*>(&Reset_Hook), "Device::Reset");
  s_present.Patch(dev, kSlotDev_Present, reinterpret_cast<void*>(&Present_Hook), "Device::Present");
  s_create_texture.Patch(dev, kSlotDev_CreateTexture, reinterpret_cast<void*>(&CreateTexture_Hook),
                         "Device::CreateTexture");
  s_create_volume.Patch(dev, kSlotDev_CreateVolumeTexture, reinterpret_cast<void*>(&CreateVolumeTexture_Hook),
                        "Device::CreateVolumeTexture");
  s_create_cube.Patch(dev, kSlotDev_CreateCubeTexture, reinterpret_cast<void*>(&CreateCubeTexture_Hook),
                      "Device::CreateCubeTexture");
  s_create_vb.Patch(dev, kSlotDev_CreateVertexBuffer, reinterpret_cast<void*>(&CreateVertexBuffer_Hook),
                    "Device::CreateVertexBuffer");
  s_create_ib.Patch(dev, kSlotDev_CreateIndexBuffer, reinterpret_cast<void*>(&CreateIndexBuffer_Hook),
                    "Device::CreateIndexBuffer");
  s_create_rt.Patch(dev, kSlotDev_CreateRenderTarget, reinterpret_cast<void*>(&CreateRenderTarget_Hook),
                    "Device::CreateRenderTarget");
  s_create_ps.Patch(dev, kSlotDev_CreatePixelShader, reinterpret_cast<void*>(&CreatePixelShader_Hook),
                    "Device::CreatePixelShader");
  if (config::Get().trace_frame_at_sec > 0 || backbuffer::Enabled()) {
    s_get_bb.Patch(dev, kSlotDev_GetBackBuffer, reinterpret_cast<void*>(&GetBackBuffer_Hook), "Device::GetBackBuffer");
  }
  if (config::Get().trace_frame_at_sec > 0 || config::Get().tonemap == config::TonemapVariant::kHdr) {
    s_set_ps.Patch(dev, kSlotDev_SetPixelShader, reinterpret_cast<void*>(&SetPixelShader_Hook),
                   "Device::SetPixelShader");
  }
  if (config::Get().trace_frame_at_sec > 0) {
    s_stretch.Patch(dev, kSlotDev_StretchRect, reinterpret_cast<void*>(&StretchRect_Hook), "Device::StretchRect");
    s_set_rt.Patch(dev, kSlotDev_SetRenderTarget, reinterpret_cast<void*>(&SetRenderTarget_Hook),
                   "Device::SetRenderTarget");
    s_clear.Patch(dev, kSlotDev_Clear, reinterpret_cast<void*>(&Clear_Hook), "Device::Clear");
    s_set_tex.Patch(dev, kSlotDev_SetTexture, reinterpret_cast<void*>(&SetTexture_Hook), "Device::SetTexture");
    s_draw.Patch(dev, kSlotDev_DrawPrimitive, reinterpret_cast<void*>(&DrawPrimitive_Hook), "Device::DrawPrimitive");
    s_draw_idx.Patch(dev, kSlotDev_DrawIndexedPrimitive, reinterpret_cast<void*>(&DrawIndexedPrimitive_Hook),
                     "Device::DrawIndexedPrimitive");
    s_draw_up.Patch(dev, kSlotDev_DrawPrimitiveUP, reinterpret_cast<void*>(&DrawPrimitiveUP_Hook),
                    "Device::DrawPrimitiveUP");
    s_draw_idx_up.Patch(dev, kSlotDev_DrawIndexedPrimitiveUP, reinterpret_cast<void*>(&DrawIndexedPrimitiveUP_Hook),
                        "Device::DrawIndexedPrimitiveUP");
  }
}
}  // namespace bl2hdr::hooks
