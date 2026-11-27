// Shared production Metal shader text for the macOS NV12 video renderer.
// Both the production renderer (metal_video_renderer.mm) and the offscreen
// GPU verification test (tests/metal_cas_renderer_test.mm) compile exactly
// this string, so the test can never drift from what the app runs.
#ifndef CROSSDESK_METAL_CAS_SHADER_SOURCE_H_
#define CROSSDESK_METAL_CAS_SHADER_SOURCE_H_

#import <Foundation/Foundation.h>

static inline NSString* ProductionNv12ShaderSource() {
  return @R"metal(
#include <metal_stdlib>
using namespace metal;

struct RasterData {
  float4 position [[position]];
  float2 texcoord;
};

vertex RasterData nv12_vertex(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[] = {
    float2(-1.0, -1.0), float2( 1.0, -1.0),
    float2(-1.0,  1.0), float2( 1.0,  1.0)
  };
  constexpr float2 texcoords[] = {
    float2(0.0, 1.0), float2(1.0, 1.0),
    float2(0.0, 0.0), float2(1.0, 0.0)
  };
  RasterData out;
  out.position = float4(positions[vertex_id], 0.0, 1.0);
  out.texcoord = texcoords[vertex_id];
  return out;
}

struct Nv12Color {
  uint full_range;
  uint bt709;
  float uv_center;
  // Contrast-adaptive sharpening strength in [0, 1]: 0 reproduces the plain
  // bilinear sample bit-for-bit. The renderer derives it from the scaling
  // ratio and subsampling; FARLINK_METAL_SHARPEN overrides it.
  float sharpness;
};

inline float nv12_luma_weighted(float y, uint full_range) {
  return full_range ? y : (y - 16.0 / 255.0) * (255.0 / 219.0);
}

// FidelityFX-style contrast-adaptive sharpening (CAS/RCAS) on luma. The tap
// pattern is the 4-neighbourhood cross one source texel around the bilinear
// footprint centre. The lobe weight is attenuated by local contrast in the
// spirit of AMD's formulation, so black-on-white UI text keeps zero ringing:
// the sharpened luma is clamped to the cross neighbourhood's [min, max]
// envelope and can never overshoot a step edge. When sharpness == 0 the
// normalizer is 1 + 4w = 1 and the result is the exact bilinear centre
// sample; flat regions have identical taps, so they stay bit-exact too.
inline float cas_sharpened_luma(float c, float n, float s, float e, float w,
                                float sharpness) {
  const float min_cross = min(min(n, s), min(e, w));
  const float max_cross = max(max(n, s), max(e, w));
  const float min_y = min(c, min_cross);
  const float max_y = max(c, max_cross);
  // RCAS/CAS contrast adaptation: soften the lobe near edges that already
  // reach black or white. A flat neighbourhood (range 0) still sharpens —
  // that case is a softened stroke interior, exactly what must crisp up.
  const float range = max_y - min_y;
  const float contrast = saturate(range * 4.0);
  float weight = -sharpness * 0.25 * (1.0 - 0.5 * contrast);
  // Never let the negative lobes overpower the centre; keeps the filter
  // numerically stable at sharpness = 1.
  weight = max(weight, -0.2);
  const float sharpened =
      (c + weight * (n + s + e + w)) / (1.0 + 4.0 * weight);
  // Hard envelope clamp: zero halo beyond the neighbourhood extremum.
  return clamp(sharpened, min_y, max_y);
}

fragment half4 nv12_fragment(
    RasterData in [[stage_in]],
    texture2d<float, access::sample> y_texture [[texture(0)]],
    texture2d<float, access::sample> uv_texture [[texture(1)]],
    constant Nv12Color& color [[buffer(0)]]) {
  constexpr sampler video_sampler(coord::normalized,
                                  address::clamp_to_edge,
                                  filter::linear);
  float y = y_texture.sample(video_sampler, in.texcoord).r;
  if (color.sharpness > 0.0) {
    // Sample the cross at one-texel source offsets around the same bilinear
    // footprint centre. clamp_to_edge keeps border texels exact.
    const float2 texel =
        1.0 / float2(y_texture.get_width(), y_texture.get_height());
    const float yn = y_texture.sample(video_sampler, in.texcoord + float2(0.0, -texel.y)).r;
    const float ys = y_texture.sample(video_sampler, in.texcoord + float2(0.0,  texel.y)).r;
    const float ye = y_texture.sample(video_sampler, in.texcoord + float2( texel.x, 0.0)).r;
    const float yw = y_texture.sample(video_sampler, in.texcoord + float2(-texel.x, 0.0)).r;
    // CAS operates on display luma, so limited-range content is expanded
    // before sharpening and the envelope clamp applies in the same space the
    // RGB coefficients consume.
    const float lc = nv12_luma_weighted(y,  color.full_range);
    const float ln = nv12_luma_weighted(yn, color.full_range);
    const float ls = nv12_luma_weighted(ys, color.full_range);
    const float le = nv12_luma_weighted(ye, color.full_range);
    const float lw = nv12_luma_weighted(yw, color.full_range);
    y = cas_sharpened_luma(lc, ln, ls, le, lw, color.sharpness);
  } else if (!color.full_range) {
    y = nv12_luma_weighted(y, color.full_range);
  }
  float2 uv = uv_texture.sample(video_sampler, in.texcoord).rg - color.uv_center;
  if (!color.full_range) {
    uv *= 255.0 / 224.0;
  }
  const float3 rgb = color.bt709
      ? float3(y + 1.5748 * uv.y, y - .187324 * uv.x - .468124 * uv.y, y + 1.8556 * uv.x)
      : float3(y + 1.402 * uv.y, y - .344136 * uv.x - .714136 * uv.y, y + 1.772 * uv.x);
  return half4(half3(saturate(rgb)), 1.0h);
}
)metal";
}

#endif  // CROSSDESK_METAL_CAS_SHADER_SOURCE_H_
