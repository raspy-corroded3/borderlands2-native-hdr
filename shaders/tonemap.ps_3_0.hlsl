// Replacement for Borderlands 2's final post-process pixel shader (UberPostProcessBlend,
// CRC32 0x54ED86A0, 1924 bytes in GlobalShaderCache-PC-D3D-SM3.bin).
// Rewritten from the original ps_3_0 disassembly (hdr/shaders/original/*.cso, fxc /dumpbin) and the
// shipped source Engine/Shaders/UberPostProcessBlendPixelShader.usf. Register bindings and inputs
// must match the original exactly, because the game sets them for the original shader.
//
// Variants (compile-time):
//   BL2HDR_TINT=1   visible test: tints the final image (milestone 3 swap test)
//   BL2HDR_HDR=1    HDR: original look up to SDR white, highlights extended above it (milestone 4)
//   (default)       replica: should be equivalent to the original

float4 BloomTintAndScreenBlendThreshold : register(c8);   // rgb bloom tint, w screen-blend threshold
float4 ImageAdjustments2 : register(c9);                  // x=A, y=B (x/(x+A)*B), z=split pos, w=linear steepness
float4 ImageAdjustments3 : register(c10);                 // x=toe factor
float4 HalfResMaskRect : register(c11);                   // clamp rect for half-res UVs
float4 DOFKernelSize : register(c12);                     // z,w used for the tunnel vignette factor
float4 VignetteSettings : register(c13);                  // x=enable threshold, y=bias
float4 VignetteColor : register(c14);

sampler2D SceneColorTexture : register(s0);               // full-res scene colour (linear, FP16)
sampler2D FilterColor1Texture : register(s1);             // bloom
sampler2D VignetteTexture : register(s2);
sampler2D ColorGradingLUT : register(s3);                 // 256x16 unwrapped 16^3 LUT
sampler2D LowResPostProcessBuffer : register(s4);         // half-res DOF/motion blur, a = blend

static const float MAX_SCENE_COLOR = 4.0;

#if BL2HDR_HDR
// Set by bl2hdr (SetPixelShaderConstantF(50)) whenever this shader is bound.
// x = peak brightness in game-white units (PeakNits / PaperWhiteNits), y = HDR strength (0 = SDR),
// z = game white / UI white (PaperWhiteNits / UIWhiteNits). The back buffer's unit is UI white: the
// HUD is drawn on top at 1.0 = UI white, so the scene is pre-scaled by z here.
float4 HdrParams : register(c50);

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Extend highlights above SDR white while keeping the graded SDR look below it.
//   sdr_gamma: final graded SDR colour (gamma space, 0..1); scene: linear scene colour before the curve.
float3 ExtendHighlights(float3 sdr_gamma, float3 scene) {
  float3 sdr_linear = pow(max(sdr_gamma, 0.0), 2.2);
  float y_sdr = Luma(sdr_linear);
  // The game's own linear segment: display_linear = scene * steepness (ImageAdjustments2.w).
  float y_target = Luma(max(scene, 0.0)) * ImageAdjustments2.w;
  // Smooth roll-off above SDR white toward the peak (both in paper-white units).
  float peak = max(HdrParams.x, 1.001);
  float over = max(y_target - 1.0, 0.0);
  float y_rolled = 1.0 + over / (1.0 + over / (peak - 1.0));
  // Only the energy above SDR white is added: continuous at white, midtones untouched.
  float y_out = y_sdr + HdrParams.y * max(y_rolled - 1.0, 0.0);
  float3 hdr_linear = sdr_linear * (y_out / max(y_sdr, 1e-4)) * HdrParams.z;  // game white -> UI-white units
  return pow(hdr_linear, 1.0 / 2.2);  // gamma space, may exceed 1.0; the output pass linearises it
}
#endif

// Gearbox/Epic tone curve: blend of x/(x+A)*B and a linear-steepness gamma curve, then clamp.
float3 TonemapAndGammaCorrect(float3 c) {
  const float A = ImageAdjustments2.x, B = ImageAdjustments2.y;
  const float split = ImageAdjustments2.z, steepness = ImageAdjustments2.w, toe = ImageAdjustments3.x;
  float3 mask = saturate((c - split) * 10000.0);
  float3 old_curve = c / abs(c + A) * B;
  float3 not_tonemapped = pow(c * steepness, 1.0 / 2.2);
  float3 flat = lerp(not_tonemapped, old_curve, mask);
  return saturate(lerp(flat, old_curve, toe));   // <-- the SDR clamp HDR will remove
}

// 2D unwrapped 16x16x16 LUT, blue slices side by side (matches the original's math exactly).
float3 ColorLookupTable(float3 g) {
  float blue_scaled = g.b * 14.9999;
  float slice = floor(blue_scaled);
  float2 uv0 = float2(slice * 0.0625 + g.r * 0.05859375 + 0.001953125, g.g * 0.9375 + 0.03125);
  float2 uv1 = uv0 + float2(0.0625, 0.0);
  float frac_b = g.b * 15.0 - slice;
  return lerp(tex2D(ColorGradingLUT, uv0).rgb, tex2D(ColorGradingLUT, uv1).rgb, frac_b);
}

float4 main(float4 uv0 : TEXCOORD0, float4 uv1 : TEXCOORD1) : COLOR0 {
  // Half-res DOF/motion-blur buffer blended in, with a tunnel vignette factor.
  float2 tunnel = (float2(uv0.z, uv0.w + DOFKernelSize.w) * 2.0 - 1.0) * DOFKernelSize.z;
  float vignette_factor = 1.0 - saturate(dot(tunnel, tunnel));
  float4 half_res = tex2D(LowResPostProcessBuffer, clamp(uv1.zw, HalfResMaskRect.xy, HalfResMaskRect.zw));
  float3 scene = tex2Dlod(SceneColorTexture, float4(uv1.xy, 0.0, 0.0)).rgb;
  float3 color = lerp(half_res.rgb * MAX_SCENE_COLOR, scene, saturate(half_res.a + vignette_factor));

  // Bloom, screen-blended by luminance.
  float luminance = dot(color, float3(0.3, 0.59, 0.11));
  float bloom_weight = saturate(exp2(luminance * -3.0) * BloomTintAndScreenBlendThreshold.w);
  float3 bloom = tex2D(FilterColor1Texture, uv0.zw).rgb * BloomTintAndScreenBlendThreshold.rgb * MAX_SCENE_COLOR;
  color += bloom * bloom_weight;

  // Coloured vignette (skipped when VignetteSettings.x <= 0.01), literal port of the original math.
  float3 tinted = color * lerp(VignetteColor.rgb, 1.0, uv1.y);
  float vignette = saturate(tex2D(VignetteTexture, uv1.xy * 2.0).x + VignetteSettings.y);
  float3 vignetted = lerp(color * tinted, color, vignette);
  color = (0.01 - VignetteSettings.x >= 0.0) ? color : vignetted;

  float3 graded = ColorLookupTable(TonemapAndGammaCorrect(color));

#if BL2HDR_TINT
  graded *= float3(1.0, 0.55, 1.0);  // obvious magenta tint: proves the swap is live
#endif
#if BL2HDR_HDR
  graded = ExtendHighlights(graded, color);
#endif
  return float4(graded, 0.0);
}
