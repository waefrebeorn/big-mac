/* metal_shaders.metal — Metal compute shaders for Big Mac compositor GPU acceleration.
 *
 * These shaders mirror the CPU effect ops in wb_compositor.c. Each kernel
 * corresponds to one effect node. Uses buffer-based compute (float4 arrays)
 * for compatibility with older Intel GPUs that don't support texture shader-write.
 *
 * More shaders (deep_fry, vhs, rgb_glitch, etc.) can be added here following
 * the same pattern. */

#include <metal_stdlib>
using namespace metal;

/* ---- sRGB decode/encode helpers (match CPU wb_lin_channel) ---- */
inline float srgb_decode(float v) {
    if (v <= 0.04045f) return v / 12.92f;
    return pow((v + 0.055f) / 1.055f, 2.4f);
}
inline float srgb_encode(float v) {
    if (v <= 0.0031308f) return v * 12.92f;
    return 1.055f * pow(v, 1.0f / 2.4f) - 0.055f;
}

/* ---- Primary color grade (mirrors CPU op 8 in wb_compositor.c) ----
 * Buffer-based: reads/writes float4 RGBA from device buffers.
 * lift/gamma/gain/saturation applied in linear light. */
kernel void grade_primary(
    device const float4 *in_buf   [[buffer(0)]],
    device float4       *out_buf  [[buffer(1)]],
    constant float4     &params   [[buffer(2)]],
    uint gid [[thread_position_in_grid]])
{
    float lift = params.x;
    float gamma = params.y;
    float gain = params.z;
    float sat = params.w;

    float4 p = in_buf[gid];

    /* ASC order: lift, then gamma+gain in linear, then encode */
    float3 c = p.rgb + float3(lift);
    c = clamp(c, 0.0f, 1.0f);

    /* decode sRGB -> linear */
    float3 lin;
    lin.r = srgb_decode(c.r);
    lin.g = srgb_decode(c.g);
    lin.b = srgb_decode(c.b);

    /* gamma + gain */
    float g = gamma > 0.001f ? gamma : 1.0f;
    float gnv = gain > 0.001f ? gain : 1.0f;
    lin = max(lin, 0.0f);
    lin = pow(lin, 1.0f / g) * gnv;

    /* encode linear -> sRGB */
    float3 enc;
    enc.r = srgb_encode(lin.r);
    enc.g = srgb_encode(lin.g);
    enc.b = srgb_encode(lin.b);

    /* saturation around Rec.709 luma (display-space) */
    float lum = 0.2126f * enc.r + 0.7152f * enc.g + 0.0722f * enc.b;
    enc = lum + (enc - lum) * sat;
    enc = clamp(enc, 0.0f, 1.0f);

    out_buf[gid] = float4(enc, p.a);
}

/* ---- Brightness gain (mirrors CPU op 1) ---- */
kernel void effect_gain(
    device const float4 *in_buf   [[buffer(0)]],
    device float4       *out_buf  [[buffer(1)]],
    constant float      &gain     [[buffer(2)]],
    uint gid [[thread_position_in_grid]])
{
    float4 p = in_buf[gid];
    p.rgb *= gain;
    p.rgb = clamp(p.rgb, 0.0f, 1.0f);
    out_buf[gid] = p;
}

/* ---- Invert alpha (mirrors CPU op 2) ---- */
kernel void effect_invert_alpha(
    device const float4 *in_buf   [[buffer(0)]],
    device float4       *out_buf  [[buffer(1)]],
    uint gid [[thread_position_in_grid]])
{
    float4 p = in_buf[gid];
    p.a = 1.0f - p.a;
    out_buf[gid] = p;
}

/* ---- White balance (mirrors CPU op 9) ---- */
kernel void effect_white_balance(
    device const float4 *in_buf   [[buffer(0)]],
    device float4       *out_buf  [[buffer(1)]],
    constant float2     &params   [[buffer(2)]],
    uint gid [[thread_position_in_grid]])
{
    float temp = params.x;
    float tint = params.y;
    float4 p = in_buf[gid];
    p.r *= 1.0f + temp;
    p.b *= 1.0f - temp;
    p.g *= 1.0f + tint;
    p.rgb = clamp(p.rgb, 0.0f, 1.0f);
    out_buf[gid] = p;
}

/* ---- Deep fry effect (GPU version of wb_deep_fry.c) ---- */
kernel void effect_deep_fry(
    device const float4 *in_buf    [[buffer(0)]],
    device float4       *out_buf   [[buffer(1)]],
    constant float4     &params    [[buffer(2)]],
    uint gid [[thread_position_in_grid]])
{
    float saturation = params.x;
    float contrast = params.y;
    float brightness = params.z;
    float noise_amt = params.w;

    float4 p = in_buf[gid];

    /* Brightness */
    p.rgb *= brightness;

    /* Contrast stretch around 0.5 (normalized float) */
    p.rgb = (p.rgb - 0.5f) * contrast + 0.5f;

    /* Saturation */
    float gray = 0.299f * p.r + 0.587f * p.g + 0.114f * p.b;
    p.rgb = gray + (p.rgb - gray) * saturation;

    /* Simple hash noise (deterministic per-pixel) */
    if (noise_amt > 0.0f) {
        uint h = gid * 73856093u ^ 19349663u;
        h = h * 1103515245u + 12345u;
        float n = float(h & 0xFFFF) / 65535.0f - 0.5f;
        p.rgb += n * noise_amt;
    }

    p.rgb = clamp(p.rgb, 0.0f, 1.0f);
    out_buf[gid] = p;
}