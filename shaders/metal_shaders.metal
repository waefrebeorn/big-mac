/* metal_shaders.metal — Metal compute shaders for Big Mac compositor GPU acceleration.
 *
 * These shaders mirror the CPU effect ops in wb_compositor.c. Each kernel
 * corresponds to one effect node. The first one implemented is the primary
 * color grade (op 8): lift/gamma/gain/saturation in linear light.
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
 * lift/gamma/gain/saturation applied in linear light, matching the CPU path
 * exactly so GPU output is bit-identical (within float precision). */
kernel void grade_primary(
    texture2d<float, access::read>  in_tex  [[texture(0)]],
    texture2d<float, access::write> out_tex [[texture(1)]],
    constant float &lift      [[buffer(0)]],
    constant float &gamma     [[buffer(1)]],
    constant float &gain      [[buffer(2)]],
    constant float &sat       [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= in_tex.get_width() || gid.y >= in_tex.get_height()) return;

    float4 p = in_tex.read(gid);

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
    /* Clamp to avoid pow(0,neg)=inf or pow(neg,frac)=NaN */
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

    out_tex.write(float4(enc, p.a), gid);
}

/* ---- Brightness gain (mirrors CPU op 1) ---- */
kernel void effect_gain(
    texture2d<float, access::read>  in_tex  [[texture(0)]],
    texture2d<float, access::write> out_tex [[texture(1)]],
    constant float &gain      [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= in_tex.get_width() || gid.y >= in_tex.get_height()) return;
    float4 p = in_tex.read(gid);
    p.rgb *= gain;
    p.rgb = clamp(p.rgb, 0.0f, 1.0f);
    out_tex.write(p, gid);
}

/* ---- Invert alpha (mirrors CPU op 2) ---- */
kernel void effect_invert_alpha(
    texture2d<float, access::read>  in_tex  [[texture(0)]],
    texture2d<float, access::write> out_tex [[texture(1)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= in_tex.get_width() || gid.y >= in_tex.get_height()) return;
    float4 p = in_tex.read(gid);
    p.a = 1.0f - p.a;
    out_tex.write(p, gid);
}

/* ---- White balance (mirrors CPU op 9) ---- */
kernel void effect_white_balance(
    texture2d<float, access::read>  in_tex  [[texture(0)]],
    texture2d<float, access::write> out_tex [[texture(1)]],
    constant float &temp      [[buffer(0)]],
    constant float &tint      [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= in_tex.get_width() || gid.y >= in_tex.get_height()) return;
    float4 p = in_tex.read(gid);
    p.r *= 1.0f + temp;
    p.b *= 1.0f - temp;
    p.g *= 1.0f + tint;
    p.rgb = clamp(p.rgb, 0.0f, 1.0f);
    out_tex.write(p, gid);
}

/* ---- Deep fry effect (simplified GPU version of wb_deep_fry.c) ----
 * Applies saturation boost, contrast stretch, and brightness in one pass.
 * The full multi-pass CPU version with noise/sharpen can be added later. */
kernel void effect_deep_fry(
    texture2d<float, access::read>  in_tex  [[texture(0)]],
    texture2d<float, access::write> out_tex [[texture(1)]],
    constant float &saturation  [[buffer(0)]],
    constant float &contrast    [[buffer(1)]],
    constant float &brightness  [[buffer(2)]],
    constant float &noise_amt   [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= in_tex.get_width() || gid.y >= in_tex.get_height()) return;
    float4 p = in_tex.read(gid);

    /* Brightness */
    p.rgb *= brightness;

    /* Contrast stretch around 0.5 (normalized float) */
    p.rgb = (p.rgb - 0.5f) * contrast + 0.5f;

    /* Saturation */
    float gray = 0.299f * p.r + 0.587f * p.g + 0.114f * p.b;
    p.rgb = gray + (p.rgb - gray) * saturation;

    /* Simple hash noise (deterministic per-pixel) */
    if (noise_amt > 0.0f) {
        uint h = gid.x * 73856093u ^ gid.y * 19349663u;
        h = h * 1103515245u + 12345u;
        float n = float(h & 0xFFFF) / 65535.0f - 0.5f;
        p.rgb += n * noise_amt;
    }

    p.rgb = clamp(p.rgb, 0.0f, 1.0f);
    out_tex.write(p, gid);
}