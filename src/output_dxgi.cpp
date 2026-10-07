// Milestone 4: present the game through our own DXGI swapchain on the D3D12 device that D3D9on12
// runs on, instead of D3D9's Present (which does not reach the screen when windowed under 9on12 on
// the test system).
//
// SDR path ([output] Format=sdr): copy the real B8G8R8A8 back buffer into a B8G8R8A8 swapchain.
// HDR path ([output] Format=hdr): the game renders into our FP16 substitute back buffer
// (backbuffer.cpp); an encode pass (shaders/encode.hlsl) converts it to scRGB in an
// R16G16B16A16_FLOAT swapchain with DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709.
//
// Per frame: UnwrapUnderlyingResource (9on12 makes our queue wait for its pending work) -> copy or
// draw on our queue -> Signal fence -> ReturnUnderlyingResource (9on12 waits on our fence) -> Present.
#include "output_dxgi.h"

#include <d3d12.h>
#include <d3d9on12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "backbuffer.h"
#include "config.h"
#include "device_removed.h"
#include "encode_ps.h"
#include "encode_vs.h"
#include "log.h"

namespace bl2hdr::output {
namespace {

constexpr UINT kBuffers = 2;

template <class T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

struct State {
  HWND hwnd = nullptr;
  UINT width = 0, height = 0;
  // Failures are retried: D3D9's own Present does not reach the screen under 9on12 (windowed), so
  // giving up for good would leave a black screen. Retries back off (in Presents) and restart on Reset.
  unsigned fail_streak = 0;
  unsigned long long presents = 0;  // Presents seen by Enabled(), for the retry back-off
  unsigned long long retry_at = 0;  // no output attempt before this Present
  bool removed = false;  // D3D12 device removed/hung: nothing of ours can run on it any more
  int enabled = -1;      // -1 unknown, 0 no, 1 yes (cached per device)
  bool hdr = false;     // HDR path active for the current swapchain
  IDirect3DDevice9On12* on12 = nullptr;
  ID3D12Device* dev = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* alloc[2] = {};
  UINT64 alloc_fence[2] = {};
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0;
  IDXGISwapChain3* swapchain = nullptr;
  UINT sc_width = 0, sc_height = 0;
  bool tearing = false;
  // HDR encode pipeline
  ID3D12RootSignature* root_sig = nullptr;
  ID3D12PipelineState* pso = nullptr;
  ID3D12DescriptorHeap* rtv_heap = nullptr;
  ID3D12DescriptorHeap* srv_heap = nullptr;
  UINT rtv_inc = 0, srv_inc = 0;
  unsigned long long frames = 0;
  int present_errors = 0;
  // automated-test capture
  std::string pending_capture;  // label of the next capture ("" = none)
  IDXGIAdapter3* adapter = nullptr;  // for VideoMemory(), created on first use
};
State g;

// ---------- capture (automated testing) ----------
struct Capture {
  ID3D12Resource* readback = nullptr;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;  // readback buffer size: the last row is not padded to RowPitch
  UINT width = 0, height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  std::string label;
};

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1Fu;
  uint32_t mant = h & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {  // subnormal
      exp = 127 - 15 + 1;
      while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
      bits = sign | (exp << 23) | ((mant & 0x3FFu) << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (mant << 13);
  } else {
    bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

// Records a copy of `src` (in COMMON state) into a new readback buffer.
bool RecordCapture(ID3D12Resource* src, Capture* cap) {
  const D3D12_RESOURCE_DESC desc = src->GetDesc();
  UINT64 total = 0;
  g.dev->GetCopyableFootprints(&desc, 0, 1, 0, &cap->footprint, nullptr, nullptr, &total);
  cap->bytes = total;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              __uuidof(ID3D12Resource), reinterpret_cast<void**>(&cap->readback));
  if (FAILED(hr)) {
    log::Error("capture: CreateCommittedResource(readback %llu bytes) failed hr=0x%08lX", total,
               static_cast<unsigned long>(hr));
    return false;
  }
  cap->width = static_cast<UINT>(desc.Width);
  cap->height = desc.Height;
  cap->format = desc.Format;
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = src;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  g.list->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION dl{};
  dl.pResource = cap->readback;
  dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dl.PlacedFootprint = cap->footprint;
  D3D12_TEXTURE_COPY_LOCATION sl{};
  sl.pResource = src;
  sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  sl.SubresourceIndex = 0;
  g.list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
  std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  g.list->ResourceBarrier(1, &b);
  return true;
}

// After the GPU finished: convert, compute statistics, write <exe dir>\bl2hdr_captures\<label>.pfm.
void FinishCapture(Capture* cap) {
  const auto& cfg = config::Get();
  uint8_t* data = nullptr;
  // Not RowPitch * height: when a row is padded (e.g. 2416 FP16 pixels -> pitch 19456) that runs past the
  // buffer and Map fails with E_INVALIDARG.
  const D3D12_RANGE read{0, static_cast<SIZE_T>(cap->bytes)};
  const HRESULT map_hr = cap->readback->Map(0, &read, reinterpret_cast<void**>(&data));
  if (FAILED(map_hr) || !data) {
    log::Error("capture: Map failed hr=0x%08lX (%ux%u, row pitch %u, device removed reason 0x%08lX)",
               static_cast<unsigned long>(map_hr), cap->width, cap->height, cap->footprint.Footprint.RowPitch,
               static_cast<unsigned long>(g.dev ? g.dev->GetDeviceRemovedReason() : 0));
    SafeRelease(cap->readback);
    return;
  }
  const UINT w = cap->width, h = cap->height;
  std::vector<float> rgb(static_cast<size_t>(w) * h * 3);
  const bool fp16 = cap->format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  double sum_l = 0.0;
  float max_l = 0.0f;
  size_t over1 = 0, over15 = 0, over2 = 0, bad = 0;
  for (UINT y = 0; y < h; ++y) {
    const uint8_t* row = data + static_cast<size_t>(y) * cap->footprint.Footprint.RowPitch;
    for (UINT x = 0; x < w; ++x) {
      float r, gg, b;
      if (fp16) {
        const auto* p = reinterpret_cast<const uint16_t*>(row) + x * 4;
        r = HalfToFloat(p[0]); gg = HalfToFloat(p[1]); b = HalfToFloat(p[2]);
      } else {  // B8G8R8A8
        const uint8_t* p = row + x * 4;
        r = p[2] / 255.0f; gg = p[1] / 255.0f; b = p[0] / 255.0f;
      }
      if (!std::isfinite(r) || !std::isfinite(gg) || !std::isfinite(b)) { ++bad; r = gg = b = 0.0f; }
      float* o = &rgb[(static_cast<size_t>(h - 1 - y) * w + x) * 3];  // PFM rows are bottom-to-top
      o[0] = r; o[1] = gg; o[2] = b;
      // Statistics in linear paper-white units (1.0 = SDR white).
      const float l = 0.2126f * std::pow(std::max(r, 0.0f), 2.2f) + 0.7152f * std::pow(std::max(gg, 0.0f), 2.2f) +
                      0.0722f * std::pow(std::max(b, 0.0f), 2.2f);
      sum_l += l;
      max_l = std::max(max_l, l);
      if (l > 1.0f) ++over1;
      if (l > 1.5f) ++over15;
      if (l > 2.0f) ++over2;
    }
  }
  cap->readback->Unmap(0, nullptr);
  SafeRelease(cap->readback);

  const double n = static_cast<double>(w) * h;
  log::Info("capture %s: %ux%u %s | luminance in buffer units (1.0 = UI white): mean %.3f, max %.2f (%.0f nits) | above SDR white "
            "%.2f%%, >1.5x %.2f%%, >2x %.2f%% | non-finite %zu",
            cap->label.c_str(), w, h, fp16 ? "FP16" : "8-bit", sum_l / n, max_l, max_l * cfg.ui_white_nits,
            100.0 * over1 / n, 100.0 * over15 / n, 100.0 * over2 / n, bad);

  wchar_t dir[MAX_PATH];
  GetModuleFileNameW(nullptr, dir, MAX_PATH);
  if (wchar_t* slash = wcsrchr(dir, L'\\')) *(slash + 1) = 0;
  wcscat_s(dir, L"bl2hdr_captures");
  CreateDirectoryW(dir, nullptr);
  std::wstring path = std::wstring(dir) + L"\\" + std::wstring(cap->label.begin(), cap->label.end()) + L".pfm";
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
    std::fprintf(f, "PF\n%u %u\n-1.0\n", w, h);
    std::fwrite(rgb.data(), sizeof(float), rgb.size(), f);
    std::fclose(f);
    log::Info("capture %s: wrote %ls (gamma-space values; 1.0 = SDR white)", cap->label.c_str(), path.c_str());
  } else {
    log::Error("capture %s: cannot write %ls", cap->label.c_str(), path.c_str());
  }
}

// True when the D3D12 device is gone (logs the reason once). `hr` is the result that raised the
// suspicion; the device's own removed reason is checked as well.
bool CheckRemoved(const char* where, HRESULT hr) {
  if (g.removed) return true;
  const HRESULT reason = g.dev ? g.dev->GetDeviceRemovedReason() : S_OK;
  const bool lost = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
                    hr == DXGI_ERROR_DEVICE_HUNG || FAILED(reason);
  if (!lost) return false;
  g.removed = true;
  log::Error("output(dxgi): GPU device removed or hung (%s: hr=0x%08lX, removed reason 0x%08lX) - our output stops",
             where, static_cast<unsigned long>(hr), static_cast<unsigned long>(reason));
  device_removed::Report(where, FAILED(reason) ? reason : hr);
  return true;
}

// Waits until the GPU reached `value`. False on timeout or device removal (the work is still pending or
// never completes): the caller must not reuse what that work references.
bool WaitForFence(UINT64 value, DWORD timeout_ms = 5000) {
  if (!g.fence || g.fence->GetCompletedValue() >= value) return true;
  if (FAILED(g.fence->SetEventOnCompletion(value, g.fence_event)) ||
      WaitForSingleObject(g.fence_event, timeout_ms) != WAIT_OBJECT_0) {
    if (!CheckRemoved("fence wait", S_OK)) {
      log::Warn("output(dxgi): GPU did not reach fence %llu within %lu ms (completed %llu)", value, timeout_ms,
                g.fence->GetCompletedValue());
    }
    return g.fence->GetCompletedValue() >= value;
  }
  return true;
}

void ReleaseAll() {
  if (g.queue && g.fence && !g.removed) {
    ++g.fence_value;
    g.queue->Signal(g.fence, g.fence_value);
    WaitForFence(g.fence_value, 10000);
  }
  SafeRelease(g.pso);
  SafeRelease(g.root_sig);
  SafeRelease(g.rtv_heap);
  SafeRelease(g.srv_heap);
  SafeRelease(g.swapchain);
  SafeRelease(g.list);
  SafeRelease(g.alloc[0]);
  SafeRelease(g.alloc[1]);
  SafeRelease(g.fence);
  SafeRelease(g.queue);
  SafeRelease(g.adapter);
  SafeRelease(g.dev);
  SafeRelease(g.on12);
  if (g.fence_event) {
    CloseHandle(g.fence_event);
    g.fence_event = nullptr;
  }
  g.alloc_fence[0] = g.alloc_fence[1] = 0;
  g.frames = 0;
}

const char* DxgiFormatName(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    default: return "other";
  }
}

// Releases everything and schedules a retry: after 1, 2, 4 ... Presents, at most every 256.
bool Fail(const char* what, HRESULT hr) {
  CheckRemoved(what, hr);
  ++g.fail_streak;
  const unsigned long long wait = 1ull << std::min(g.fail_streak - 1, 8u);
  g.retry_at = g.presents + wait;
  if (g.fail_streak <= 5 || g.fail_streak % 50 == 0) {
    log::Error("output(dxgi): %s failed hr=0x%08lX (failure %u in a row) - %s", what, static_cast<unsigned long>(hr),
               g.fail_streak, g.removed ? "device removed, not retrying" : "retrying shortly");
  }
  ReleaseAll();
  return false;
}

bool CreateEncodePipeline() {
  using Serialize_t = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                       ID3DBlob**);
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  if (!d3d12) d3d12 = LoadLibraryW(L"d3d12.dll");
  auto serialize = d3d12 ? reinterpret_cast<Serialize_t>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
  if (!serialize) return Fail("D3D12SerializeRootSignature lookup", E_NOINTERFACE);

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 4};
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob* blob = nullptr;
  ID3DBlob* err = nullptr;
  HRESULT hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
  if (err) {
    log::Error("output(dxgi): root signature: %s", static_cast<const char*>(err->GetBufferPointer()));
    err->Release();
  }
  if (FAILED(hr)) return Fail("D3D12SerializeRootSignature", hr);
  hr = g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                  reinterpret_cast<void**>(&g.root_sig));
  blob->Release();
  if (FAILED(hr)) return Fail("CreateRootSignature", hr);

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = g.root_sig;
  pd.VS = {g_encode_vs, sizeof(g_encode_vs)};
  pd.PS = {g_encode_ps, sizeof(g_encode_ps)};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
  pd.SampleDesc.Count = 1;
  hr = g.dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&g.pso));
  if (FAILED(hr)) return Fail("CreateGraphicsPipelineState", hr);

  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBuffers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  hr = g.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g.rtv_heap));
  if (FAILED(hr)) return Fail("CreateDescriptorHeap(RTV)", hr);
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  hr = g.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g.srv_heap));
  if (FAILED(hr)) return Fail("CreateDescriptorHeap(SRV)", hr);
  g.rtv_inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  g.srv_inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (UINT i = 0; i < kBuffers; ++i) {
    ID3D12Resource* buf = nullptr;
    g.swapchain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buf));
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(i) * g.rtv_inc;
    g.dev->CreateRenderTargetView(buf, nullptr, h);
    buf->Release();
  }
  log::Info("output(dxgi): HDR encode pipeline ready (root signature, PSO, RTV/SRV heaps)");
  return true;
}

// Size of the image we present: the FP16 substitute (HDR) or the real back buffer (SDR).
void SourceSize(IDirect3DDevice9* device, UINT* w, UINT* h) {
  D3DSURFACE_DESC d{};
  if (IDirect3DSurface9* sub = backbuffer::Surface()) {
    sub->GetDesc(&d);
  } else {
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
      bb->GetDesc(&d);
      bb->Release();
    }
  }
  *w = d.Width;
  *h = d.Height;
}

bool Init(IDirect3DDevice9* device) {
  g.hdr = config::Get().output_hdr && backbuffer::Texture() != nullptr;
  HRESULT hr = device->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&g.on12));
  if (FAILED(hr)) return Fail("QueryInterface(IDirect3DDevice9On12)", hr);
  hr = g.on12->GetD3D12Device(__uuidof(ID3D12Device), reinterpret_cast<void**>(&g.dev));
  if (FAILED(hr)) return Fail("GetD3D12Device", hr);

  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  hr = g.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&g.queue));
  if (FAILED(hr)) return Fail("CreateCommandQueue", hr);
  for (auto& a : g.alloc) {
    hr = g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                       reinterpret_cast<void**>(&a));
    if (FAILED(hr)) return Fail("CreateCommandAllocator", hr);
  }
  hr = g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr,
                                __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&g.list));
  if (FAILED(hr)) return Fail("CreateCommandList", hr);
  g.list->Close();
  hr = g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g.fence));
  if (FAILED(hr)) return Fail("CreateFence", hr);
  g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  using CreateFactory2_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  auto create_factory = dxgi ? reinterpret_cast<CreateFactory2_t>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
  if (!create_factory) return Fail("CreateDXGIFactory2 lookup", E_NOINTERFACE);
  IDXGIFactory4* factory = nullptr;
  hr = create_factory(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
  if (FAILED(hr)) return Fail("CreateDXGIFactory2", hr);
  IDXGIFactory5* factory5 = nullptr;
  if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5)))) {
    BOOL allow = FALSE;
    if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
      g.tearing = allow != FALSE;
    }
    factory5->Release();
  }

  // Size the swapchain like the game's image (not the window): DXGI_SCALING_STRETCH maps it to the
  // window, and a minimized window (client rect 0x0) cannot break creation.
  UINT w = 0, h = 0;
  SourceSize(device, &w, &h);
  if (w == 0 || h == 0) { w = g.width; h = g.height; }

  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = w;
  sd.Height = h;
  sd.Format = g.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kBuffers;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
  sd.Flags = g.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
  IDXGISwapChain1* sc1 = nullptr;
  hr = factory->CreateSwapChainForHwnd(g.queue, g.hwnd, &sd, nullptr, nullptr, &sc1);
  if (FAILED(hr)) {
    factory->Release();
    return Fail("CreateSwapChainForHwnd", hr);
  }
  factory->MakeWindowAssociation(g.hwnd, DXGI_MWA_NO_ALT_ENTER);
  factory->Release();
  hr = sc1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&g.swapchain));
  sc1->Release();
  if (FAILED(hr)) return Fail("QueryInterface(IDXGISwapChain3)", hr);
  g.sc_width = w;
  g.sc_height = h;

  if (g.hdr) {
    const DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;  // scRGB
    UINT support = 0;
    g.swapchain->CheckColorSpaceSupport(cs, &support);
    hr = (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ? g.swapchain->SetColorSpace1(cs) : E_FAIL;
    log::Info("output(dxgi): scRGB colour space support=0x%X, SetColorSpace1 hr=0x%08lX", support,
              static_cast<unsigned long>(hr));
    // Not fatal: scRGB is already the default colour space of an FP16 flip-model swapchain.
    if (FAILED(hr)) log::Warn("output(dxgi): SetColorSpace1(scRGB) not accepted - using the FP16 default (scRGB)");
    if (!CreateEncodePipeline()) return false;
  }

  log::Info("output(dxgi): swapchain created on hwnd %p: %ux%u %s, %u buffers, FLIP_DISCARD, tearing=%d, path=%s",
            static_cast<void*>(g.hwnd), w, h, DxgiFormatName(sd.Format), kBuffers, g.tearing, g.hdr ? "HDR" : "SDR");
  return true;
}

void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
             D3D12_RESOURCE_BARRIER* out) {
  *out = {};
  out->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  out->Transition.pResource = r;
  out->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  out->Transition.StateBefore = before;
  out->Transition.StateAfter = after;
}

void RecordCopy(ID3D12Resource* src, ID3D12Resource* dst) {
  const D3D12_RESOURCE_DESC sdesc = src->GetDesc();
  const D3D12_RESOURCE_DESC ddesc = dst->GetDesc();
  D3D12_RESOURCE_BARRIER b[2];
  Barrier(src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE, &b[0]);
  Barrier(dst, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST, &b[1]);
  g.list->ResourceBarrier(2, b);
  if (sdesc.Width == ddesc.Width && sdesc.Height == ddesc.Height && sdesc.Format == ddesc.Format) {
    g.list->CopyResource(dst, src);
  } else {
    D3D12_TEXTURE_COPY_LOCATION dl{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_BOX box{0, 0, 0, static_cast<UINT>(std::min(sdesc.Width, ddesc.Width)), std::min(sdesc.Height, ddesc.Height),
                  1};
    if (g.frames == 0) log::Warn("output(dxgi): size/format mismatch - copying the overlapping region");
    g.list->CopyTextureRegion(&dl, 0, 0, 0, &sl, &box);
  }
  Barrier(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON, &b[0]);
  Barrier(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT, &b[1]);
  g.list->ResourceBarrier(2, b);
}

void RecordEncode(ID3D12Resource* src, ID3D12Resource* dst, UINT buffer_index, UINT slot) {
  D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = g.srv_heap->GetCPUDescriptorHandleForHeapStart();
  srv_cpu.ptr += static_cast<SIZE_T>(slot) * g.srv_inc;
  D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = g.srv_heap->GetGPUDescriptorHandleForHeapStart();
  srv_gpu.ptr += static_cast<UINT64>(slot) * g.srv_inc;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  g.dev->CreateShaderResourceView(src, &sv, srv_cpu);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += static_cast<SIZE_T>(buffer_index) * g.rtv_inc;

  D3D12_RESOURCE_BARRIER b[2];
  Barrier(src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &b[0]);
  Barrier(dst, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET, &b[1]);
  g.list->ResourceBarrier(2, b);

  const auto& cfg = config::Get();
  const float constants[4] = {cfg.ui_white_nits / 80.0f, 2.2f, cfg.peak_nits / 80.0f,
                              cfg.test_pattern ? 2.0f : (cfg.debug_highlights ? 1.0f : 0.0f)};
  g.list->SetGraphicsRootSignature(g.root_sig);
  g.list->SetDescriptorHeaps(1, &g.srv_heap);
  g.list->SetGraphicsRootDescriptorTable(0, srv_gpu);
  g.list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
  const D3D12_VIEWPORT vp{0.f, 0.f, static_cast<float>(g.sc_width), static_cast<float>(g.sc_height), 0.f, 1.f};
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(g.sc_width), static_cast<LONG>(g.sc_height)};
  g.list->RSSetViewports(1, &vp);
  g.list->RSSetScissorRects(1, &scissor);
  g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g.list->DrawInstanced(3, 1, 0, 0);

  Barrier(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON, &b[0]);
  Barrier(dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT, &b[1]);
  g.list->ResourceBarrier(2, b);
}

}  // namespace

void SetWindow(HWND hwnd, UINT width, UINT height) {
  g.hwnd = hwnd;
  g.width = width;
  g.height = height;
}

HWND Window() { return g.hwnd; }

bool Enabled(IDirect3DDevice9* device) {
  if (!config::Get().UseDxgiOutput() || g.removed) return false;
  if (++g.presents < g.retry_at) return false;  // backing off after a failure
  if (g.enabled < 0) {
    IDirect3DDevice9On12* on12 = nullptr;
    g.enabled = SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&on12))) ? 1 : 0;
    if (on12) on12->Release();
    log::Info("output(dxgi): %s", g.enabled ? "enabled (device is 9on12)" : "NOT enabled - device is not 9on12 (need [d3d9] Mode=9on12)");
  }
  return g.enabled == 1;
}

HRESULT Present(IDirect3DDevice9* device) {
  // Rebuild the swapchain when the game's image size (or the HDR/SDR path) changed, e.g. after a
  // resolution change.
  if (g.swapchain) {
    UINT w = 0, h = 0;
    SourceSize(device, &w, &h);
    const bool want_hdr = config::Get().output_hdr && backbuffer::Texture() != nullptr;
    if ((w && h && (w != g.sc_width || h != g.sc_height)) || want_hdr != g.hdr) {
      log::Info("output(dxgi): source changed to %ux%u (%s) from %ux%u (%s) - recreating the swapchain", w, h,
                want_hdr ? "HDR" : "SDR", g.sc_width, g.sc_height, g.hdr ? "HDR" : "SDR");
      ReleaseAll();
    }
  }
  if (!g.swapchain && !Init(device)) return E_FAIL;

  // The command allocator for this frame must be idle before it is reset. If the GPU is that far behind
  // (or gone), drop this frame instead of resetting memory the GPU may still read.
  const UINT a = static_cast<UINT>(g.frames % 2);
  if (!WaitForFence(g.alloc_fence[a])) {
    if (g.removed) ReleaseAll();
    return E_FAIL;
  }

  // Source: the FP16 substitute back buffer (HDR) or the real back buffer (SDR).
  IDirect3DResource9* src9 = nullptr;
  IDirect3DSurface9* bb = nullptr;
  if (g.hdr) {
    src9 = backbuffer::Texture();
    src9->AddRef();
  } else {
    HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
      Fail("GetBackBuffer", hr);
      return E_FAIL;
    }
    src9 = bb;
  }

  ID3D12Resource* src = nullptr;
  HRESULT hr = g.on12->UnwrapUnderlyingResource(src9, g.queue, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&src));
  if (FAILED(hr) || !src) {
    src9->Release();
    Fail(g.hdr ? "UnwrapUnderlyingResource(substitute back buffer)" : "UnwrapUnderlyingResource(back buffer)", hr);
    return E_FAIL;
  }

  const UINT index = g.swapchain->GetCurrentBackBufferIndex();
  ID3D12Resource* dst = nullptr;
  hr = g.swapchain->GetBuffer(index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&dst));
  if (FAILED(hr) || !dst) {
    g.on12->ReturnUnderlyingResource(src9, 0, nullptr, nullptr);  // nothing of ours was queued on it
    src->Release();
    src9->Release();
    Fail("IDXGISwapChain::GetBuffer", hr);
    return E_FAIL;
  }
  if (g.frames == 0) {
    const D3D12_RESOURCE_DESC sdesc = src->GetDesc();
    const D3D12_RESOURCE_DESC ddesc = dst->GetDesc();
    log::Info("output(dxgi): source as D3D12: %llux%u %s (fmt %d), flags 0x%X; swapchain buffer %llux%u %s",
              sdesc.Width, sdesc.Height, DxgiFormatName(sdesc.Format), sdesc.Format, sdesc.Flags, ddesc.Width,
              ddesc.Height, DxgiFormatName(ddesc.Format));
  }

  g.alloc[a]->Reset();
  g.list->Reset(g.alloc[a], g.hdr ? g.pso : nullptr);
  if (g.hdr) {
    RecordEncode(src, dst, index, a);
  } else {
    RecordCopy(src, dst);
  }
  Capture cap;
  bool capturing = false;
  if (!g.pending_capture.empty()) {
    cap.label = g.pending_capture;
    g.pending_capture.clear();
    capturing = RecordCapture(src, &cap);
  }
  g.list->Close();
  ID3D12CommandList* lists[] = {g.list};
  g.queue->ExecuteCommandLists(1, lists);
  ++g.fence_value;
  g.queue->Signal(g.fence, g.fence_value);
  g.alloc_fence[a] = g.fence_value;

  // Hand the resource back to 9on12; it waits for our work before rendering into it again.
  hr = g.on12->ReturnUnderlyingResource(src9, 1, &g.fence_value, &g.fence);
  if (FAILED(hr) && g.frames == 0) {
    log::Error("output(dxgi): ReturnUnderlyingResource failed hr=0x%08lX", static_cast<unsigned long>(hr));
  }
  src->Release();
  dst->Release();
  src9->Release();

  hr = g.swapchain->Present(0, g.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
  if (FAILED(hr)) {
    if (CheckRemoved("IDXGISwapChain::Present", hr)) {
      SafeRelease(cap.readback);
      ReleaseAll();
      return E_FAIL;  // the game's own Present reports the lost device to it
    }
    if (g.present_errors++ < 5) log::Error("output(dxgi): Present failed hr=0x%08lX", static_cast<unsigned long>(hr));
  }
  if (g.frames == 0) log::Info("output(dxgi): first frame presented hr=0x%08lX", static_cast<unsigned long>(hr));
  if (g.fail_streak > 0) {
    log::Info("output(dxgi): output recovered after %u failure(s)", g.fail_streak);
    g.fail_streak = 0;
  }
  if (capturing) {
    // One-off stall: the copy must be finished before mapping.
    if (WaitForFence(g.alloc_fence[a], 10000)) {
      FinishCapture(&cap);
    } else {
      SafeRelease(cap.readback);
    }
  }
  ++g.frames;
  return D3D_OK;
}

bool VideoMemory(unsigned long long* usage, unsigned long long* budget) {
  if (!g.dev) return false;
  if (!g.adapter) {
    using CreateFactory2_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    auto create_factory = dxgi ? reinterpret_cast<CreateFactory2_t>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
    IDXGIFactory4* factory = nullptr;
    if (!create_factory || FAILED(create_factory(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) {
      return false;
    }
    factory->EnumAdapterByLuid(g.dev->GetAdapterLuid(), __uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&g.adapter));
    factory->Release();
    if (!g.adapter) return false;
  }
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  if (FAILED(g.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return false;
  *usage = info.CurrentUsage;
  *budget = info.Budget;
  return true;
}

void RequestCapture(const std::string& label) {
  if (!config::Get().UseDxgiOutput()) {
    log::Warn("capture %s requested but [output] Mode is not dxgi - skipped", label.c_str());
    return;
  }
  g.pending_capture = label;
}

void OnReset() {
  if (g.swapchain || g.on12) log::Info("output(dxgi): releasing swapchain for device Reset");
  ReleaseAll();
  g.enabled = -1;
  g.fail_streak = 0;  // a Reset is a fresh start: retry immediately
  g.retry_at = 0;
}

}  // namespace bl2hdr::output
