#pragma once
#include <d3d9.h>

namespace bl2hdr::hooks {
// Patch the IDirect3D9 vtable (CreateDevice) of this object's class. Idempotent per vtable.
void HookDirect3D9(IDirect3D9* d3d);
// Patch the IDirect3DDevice9 vtable (Present, Reset, resource/shader creation). Idempotent per vtable.
void HookDevice(IDirect3DDevice9* device);
}  // namespace bl2hdr::hooks
