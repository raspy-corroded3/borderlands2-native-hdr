// Replacement for Borderlands 2's Bink movie pixel shader (YUV -> RGB, CRC32 0x33244F80, 540 bytes in
// GlobalShaderCache-PC-D3D-SM3.bin), rewritten from its ps_3_0 disassembly. Register bindings must match
// the original exactly, because the game sets them for the original shader.
//
// The original writes the colour-matrix result unclamped. Into the game's 8-bit back buffer that clamps
// to 0..1 on its own; into bl2hdr's FP16 substitute it does not, and limited-range video overshoots
// (up to ~1.27 x white, and below 0), which made bright movie frames brighter than vanilla. The only
// change here is saturate(), so movies look exactly like vanilla (SDR, at UIWhiteNits).

float4 YuvToR : register(c0);  // row of the YUV(+1) -> RGB matrix
float4 YuvToG : register(c1);
float4 YuvToB : register(c2);
float4 Consts : register(c3);  // x = 1 (matrix offset term), w = output alpha

sampler2D TexY : register(s0);
sampler2D TexChromaA : register(s1);  // chroma planes (half resolution)
sampler2D TexChromaB : register(s2);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  const float4 yuv1 = float4(tex2D(TexY, uv).x, tex2D(TexChromaA, uv).x, tex2D(TexChromaB, uv).x, Consts.x);
  return float4(saturate(float3(dot(YuvToR, yuv1), dot(YuvToG, yuv1), dot(YuvToB, yuv1))), Consts.w);
}
