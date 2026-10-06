#pragma once
/* Vtable slots, computed in vtable_index.c from d3d9.h. */
#ifdef __cplusplus
extern "C" {
#endif
extern const unsigned kSlotD3D9_CreateDevice;
extern const unsigned kSlotDev_Reset;
extern const unsigned kSlotDev_Present;
extern const unsigned kSlotDev_CreateTexture;
extern const unsigned kSlotDev_CreateVolumeTexture;
extern const unsigned kSlotDev_CreateCubeTexture;
extern const unsigned kSlotDev_CreateVertexBuffer;
extern const unsigned kSlotDev_CreateIndexBuffer;
extern const unsigned kSlotDev_CreateRenderTarget;
extern const unsigned kSlotDev_CreatePixelShader;
extern const unsigned kSlotDev_GetBackBuffer;
extern const unsigned kSlotDev_StretchRect;
extern const unsigned kSlotDev_SetRenderTarget;
extern const unsigned kSlotDev_Clear;
extern const unsigned kSlotDev_SetPixelShader;
extern const unsigned kSlotDev_DrawPrimitive;
extern const unsigned kSlotDev_DrawIndexedPrimitive;
extern const unsigned kSlotDev_DrawPrimitiveUP;
extern const unsigned kSlotDev_DrawIndexedPrimitiveUP;
extern const unsigned kSlotDev_SetTexture;
#ifdef __cplusplus
}
#endif
