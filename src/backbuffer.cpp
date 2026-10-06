#include "backbuffer.h"

#include "config.h"
#include "log.h"

namespace bl2hdr::backbuffer {
namespace {
IDirect3DTexture9* g_tex = nullptr;
IDirect3DSurface9* g_surf = nullptr;
}  // namespace

bool Enabled() { return config::Get().UseDxgiOutput() && config::Get().output_hdr; }

void Create(IDirect3DDevice9* device, UINT width, UINT height) {
  Release();
  if (!Enabled() || width == 0 || height == 0) return;
  HRESULT hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT,
                                     &g_tex, nullptr);
  if (FAILED(hr) || !g_tex) {
    log::Error("backbuffer: CreateTexture %ux%u A16B16G16R16F failed hr=0x%08lX - HDR back buffer disabled", width,
               height, static_cast<unsigned long>(hr));
    g_tex = nullptr;
    return;
  }
  g_tex->GetSurfaceLevel(0, &g_surf);
  log::Info("backbuffer: substitute %ux%u A16B16G16R16F created (texture %p, surface %p)", width, height,
            static_cast<void*>(g_tex), static_cast<void*>(g_surf));
}

void Release() {
  if (g_surf) { g_surf->Release(); g_surf = nullptr; }
  if (g_tex) {
    const ULONG refs = g_tex->Release();
    if (refs != 0) log::Warn("backbuffer: substitute texture still has %lu reference(s) at release", refs);
    g_tex = nullptr;
  }
}

IDirect3DSurface9* Surface() { return g_surf; }
IDirect3DTexture9* Texture() { return g_tex; }
}  // namespace bl2hdr::backbuffer
