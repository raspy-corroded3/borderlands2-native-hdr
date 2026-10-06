#pragma once
// Substitute back buffer (milestone 4 HDR stage): an A16B16G16R16F render-target texture handed to
// the game from GetBackBuffer, so the tonemap pass and the HUD keep values above 1.0.
#include <d3d9.h>

namespace bl2hdr::backbuffer {
bool Enabled();                                 // [output] Mode=dxgi + Format=hdr
void Create(IDirect3DDevice9* device, UINT width, UINT height);
void Release();                                 // before Reset / on shutdown
IDirect3DSurface9* Surface();                   // level 0 of the texture (no AddRef)
IDirect3DTexture9* Texture();                   // the texture (no AddRef)
}  // namespace bl2hdr::backbuffer
