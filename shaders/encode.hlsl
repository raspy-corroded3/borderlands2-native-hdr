// Final encode pass for the D3D12 output (milestone 4, HDR stage).
// Input: the game's substituted FP16 back buffer, holding gamma-encoded (≈2.2) values: 0..1 is the
// SDR range; values above 1.0 are HDR highlights (once the HDR tonemapper is active).
// Output: scRGB (linear, BT.709 primaries, 1.0 = 80 nits) for an R16G16B16A16_FLOAT swapchain.

cbuffer EncodeConstants : register(b0) {
  float paper_white_scale;  // UIWhiteNits / 80 (buffer unit = UI white; the scene is pre-scaled by the tonemapper)
  float input_gamma;        // 2.2
  float peak_scale;         // PeakNits / 80 (clamp for safety)
  float debug_mode;         // 0 = normal, 1 = values above 1.0 in magenta, 2 = peak-brightness test pattern
};

Texture2D<float4> game_image : register(t0);

struct VSOut {
  float4 pos : SV_POSITION;
};

// Full-screen triangle from the vertex id; no vertex buffer needed.
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}

// Peak-brightness clipping test: 4x4 tiles, outer square at 10,000 nits (always the display's maximum),
// inner square at 300, 350, ... 1050 nits (row-major, top-left = 300). An inner square is visible only while
// its value is below the display's real peak; the last visible one ~ the peak. Not clamped to PeakNits.
float3 TestPattern(float2 p) {
  uint w, h;
  game_image.GetDimensions(w, h);
  float tile = min(w, h) / 5.0;
  float2 origin = (float2(w, h) - tile * 4.0) * 0.5;
  float2 q = (p - origin) / tile;
  if (any(q < 0.0) || any(q >= 4.0)) return 0.0;
  float2 cell = floor(q);
  float2 f = frac(q);
  if (any(f < 0.06) || any(f > 0.94)) return 0.0;                 // black gaps between tiles
  float index = cell.y * 4.0 + cell.x;
  float inner_nits = 300.0 + 50.0 * index;
  bool inner = all(abs(f - 0.5) < 0.2);
  return (inner ? inner_nits : 10000.0) / 80.0;
}

float4 ps_main(VSOut i) : SV_TARGET {
  if (debug_mode > 1.5) return float4(TestPattern(i.pos.xy), 1.0);
  // Negative and NaN values (bad game pixels) become 0: SM5 max() returns the non-NaN operand.
  float3 c = max(game_image.Load(int3(i.pos.xy, 0)).rgb, 0.0);
  float3 linear_color = pow(c, input_gamma) * paper_white_scale;
  linear_color = min(linear_color, peak_scale);
  if (debug_mode > 0.5 && max(c.r, max(c.g, c.b)) > 1.0) linear_color = float3(peak_scale, 0.0, peak_scale);
  return float4(linear_color, 1.0);
}
