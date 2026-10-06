/* Vtable slot indices computed from the official d3d9.h C interface (CINTERFACE), so the C++ hooks
 * never rely on hand-counted method numbers. */
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <d3d9.h>

#include "vtable_index.h"

#define SLOT(vtbl, method) ((unsigned)(offsetof(vtbl, method) / sizeof(void*)))

const unsigned kSlotD3D9_CreateDevice = SLOT(IDirect3D9Vtbl, CreateDevice);

const unsigned kSlotDev_Reset = SLOT(IDirect3DDevice9Vtbl, Reset);
const unsigned kSlotDev_Present = SLOT(IDirect3DDevice9Vtbl, Present);
const unsigned kSlotDev_CreateTexture = SLOT(IDirect3DDevice9Vtbl, CreateTexture);
const unsigned kSlotDev_CreateVolumeTexture = SLOT(IDirect3DDevice9Vtbl, CreateVolumeTexture);
const unsigned kSlotDev_CreateCubeTexture = SLOT(IDirect3DDevice9Vtbl, CreateCubeTexture);
const unsigned kSlotDev_CreateVertexBuffer = SLOT(IDirect3DDevice9Vtbl, CreateVertexBuffer);
const unsigned kSlotDev_CreateIndexBuffer = SLOT(IDirect3DDevice9Vtbl, CreateIndexBuffer);
const unsigned kSlotDev_CreateRenderTarget = SLOT(IDirect3DDevice9Vtbl, CreateRenderTarget);
const unsigned kSlotDev_CreatePixelShader = SLOT(IDirect3DDevice9Vtbl, CreatePixelShader);

/* Frame trace (milestone 4 HDR stage) */
const unsigned kSlotDev_GetBackBuffer = SLOT(IDirect3DDevice9Vtbl, GetBackBuffer);
const unsigned kSlotDev_StretchRect = SLOT(IDirect3DDevice9Vtbl, StretchRect);
const unsigned kSlotDev_SetRenderTarget = SLOT(IDirect3DDevice9Vtbl, SetRenderTarget);
const unsigned kSlotDev_Clear = SLOT(IDirect3DDevice9Vtbl, Clear);
const unsigned kSlotDev_SetPixelShader = SLOT(IDirect3DDevice9Vtbl, SetPixelShader);
const unsigned kSlotDev_DrawPrimitive = SLOT(IDirect3DDevice9Vtbl, DrawPrimitive);
const unsigned kSlotDev_DrawIndexedPrimitive = SLOT(IDirect3DDevice9Vtbl, DrawIndexedPrimitive);
const unsigned kSlotDev_DrawPrimitiveUP = SLOT(IDirect3DDevice9Vtbl, DrawPrimitiveUP);
const unsigned kSlotDev_DrawIndexedPrimitiveUP = SLOT(IDirect3DDevice9Vtbl, DrawIndexedPrimitiveUP);
const unsigned kSlotDev_SetTexture = SLOT(IDirect3DDevice9Vtbl, SetTexture);
