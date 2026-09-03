/* wb_ytp_fx.c — YTP/YTPMV effects engine (R094).
 *
 * Dark arts + poopisms + geometric warps + MIDI + automation curves.
 * Pure C11 pixel-level processing where possible, ffmpeg wrappers where needed.
 *
 * Techniques implemented:
 * - Video stutter loop (repeat video segment N times)
 * - Cookie cutter masks (circle, triangle, star, custom)
 * - Mirror / kaleidoscope
 * - Swirl / spherize / wave displacement
 * - Zoom punch / impact frame / frame freeze
 * - Scramble / random chop
 * - Strobe / flash frame
 * - CRT / scanlines
 * - Recursion (Droste)
 * - All keyframe interpolation types (bezier, elastic, bounce, TCB)
 * - All automation fade curves (exp, log, S-curve)
 * - MIDI: aftertouch, step sequencer, euclidean, probability, ratchet
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ================================================================
 * KEYFRAME INTERPOLATION TYPES (K1-K15)
 * All take t in [0,1], return value in [0,1]
 * ================================================================ */

/* K1: Constant / Step */
float kf_constant(float t) {
    return (t >= 1.0f) ? 1.0f : 0.0f;
}

/* K2: Linear */
float kf_linear(float t) {
    return t;
}

/* K3: Cubic Bezier (with control points) */
float kf_bezier(float t, float x1, float y1, float x2, float y2) {
    /* Newton-Raphson to find t for given x, then evaluate y */
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 20; i++) {
        float mid = (lo + hi) * 0.5f;
        float x = 3.0f * x1 * mid * (1.0f - mid) * (1.0f - mid)
                + 3.0f * x2 * mid * mid * (1.0f - mid)
                + mid * mid * mid;
        if (x < t) lo = mid; else hi = mid;
    }
    float mt = 1.0f - lo;
    return 3.0f * y1 * lo * mt * mt + 3.0f * y2 * lo * lo * mt + lo * lo * lo;
}

/* K4: Hermite (Catmull-Rom) */
float kf_hermite(float t, float p0, float p1, float p2, float p3) {
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) +
                   (-p0 + p2) * t +
                   (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3) * t2 +
                   (-p0 + 3.0f*p1 - 3.0f*p2 + p3) * t3);
}

/* K5: Hold (step at midpoint) */
float kf_hold(float t) {
    return (t < 0.5f) ? 0.0f : 1.0f;
}

/* K6: Ease-In (quadratic) */
float kf_ease_in(float t) {
    return t * t;
}

/* K7: Ease-Out (quadratic) */
float kf_ease_out(float t) {
    return 1.0f - (1.0f - t) * (1.0f - t);
}

/* K8: Ease-In-Out (smoothstep) */
float kf_ease_inout(float t) {
    return t * t * (3.0f - 2.0f * t);
}

/* K9: Elastic (spring ODE approximation) */
float kf_elastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    float p = 0.3f;
    return powf(2.0f, -10.0f * t) * sinf((t - p * 0.25f) * 2.0f * M_PI / p) + 1.0f;
}

/* K10: Bounce (gravity simulation) */
float kf_bounce(float t) {
    if (t < 1.0f / 2.75f) {
        return 7.5625f * t * t;
    } else if (t < 2.0f / 2.75f) {
        t -= 1.5f / 2.75f;
        return 7.5625f * t * t + 0.75f;
    } else if (t < 2.5f / 2.75f) {
        t -= 2.25f / 2.75f;
        return 7.5625f * t * t + 0.9375f;
    } else {
        t -= 2.625f / 2.75f;
        return 7.5625f * t * t + 0.984375f;
    }
}

/* K11: Back (overshoot cubic) */
float kf_back(float t) {
    float s = 1.70158f;
    return t * t * ((s + 1.0f) * t - s);
}

/* K12: Exponential */
float kf_exponential(float t) {
    if (t <= 0.0f) return 0.0f;
    return powf(2.0f, 10.0f * (t - 1.0f));
}

/* K13: Logarithmic */
float kf_logarithmic(float t) {
    if (t <= 0.0f) return 0.0f;
    return logf(1.0f + 9.0f * t) / logf(10.0f);
}

/* K14: S-Curve (smootherstep) */
float kf_scurve(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/* K15: TCB (Tension/Continuity/Bias Hermite) */
float kf_tcb(float t, float tension, float continuity, float bias) {
    /* TCB Hermite basis */
    float p0 = 0.0f, p1 = 0.0f, p2 = 1.0f, p3 = 1.0f; /* endpoints */
    float t2 = t * t;
    float t3 = t2 * t;

    float m0, m1;
    float ad = (1.0f - tension) * (1.0f + continuity) * (1.0f + bias) * 0.5f;
    float ac = (1.0f - tension) * (1.0f - continuity) * (1.0f - bias) * 0.5f;
    float bd = (1.0f - tension) * (1.0f - continuity) * (1.0f + bias) * 0.5f;
    float bc = (1.0f - tension) * (1.0f + continuity) * (1.0f - bias) * 0.5f;

    m0 = ad * (p1 - p0) + ac * (p2 - p0) + bd * (p3 - p1) + bc * (p3 - p1);
    m1 = ad * (p2 - p1) + ac * (p3 - p1) + bd * (p2 - p0) + bc * (p3 - p0);

    /* Evaluate cubic */
    float h00 = 2.0f*t3 - 3.0f*t2 + 1.0f;
    float h10 = t3 - 2.0f*t2 + t;
    float h01 = -2.0f*t3 + 3.0f*t2;
    float h11 = t3 - t2;

    return h00 * p1 + h10 * m0 + h01 * p2 + h11 * m1;
}

/* Generic interpolator selector */
float kf_interpolate(float t, int type, float p1, float p2, float p3) {
    switch (type) {
        case 0: return kf_hold(t);      /* WB_KF_HOLD */
        case 1: return kf_linear(t);     /* WB_KF_LINEAR */
        case 2: return kf_bezier(t, p1, p2, p3, 1.0f-p1); /* WB_KF_BEZIER */
        case 3: return kf_hermite(t, 0.0f, 0.0f, 1.0f, 1.0f); /* WB_KF_HERMITE */
        case 4: return kf_ease_in(t);    /* WB_KF_EASE_IN */
        case 5: return kf_ease_out(t);   /* WB_KF_EASE_OUT */
        case 6: return kf_ease_inout(t); /* WB_KF_EASE_INOUT */
        case 7: return kf_elastic(t);    /* WB_KF_ELASTIC */
        case 8: return kf_bounce(t);     /* WB_KF_BOUNCE */
        case 9: return kf_back(t);       /* WB_KF_BACK */
        case 10: return kf_exponential(t); /* WB_KF_EXPONENTIAL */
        case 11: return kf_logarithmic(t); /* WB_KF_LOGARITHMIC */
        case 12: return kf_scurve(t);    /* WB_KF_SCURVE */
        case 13: return kf_tcb(t, p1, p2, p3); /* WB_KF_TCB */
        default: return kf_linear(t);
    }
}

/* ================================================================
 * FADE CURVES (for automation)
 * ================================================================ */

float fade_linear(float t) { return t; }
float fade_exponential(float t) { return t <= 0 ? 0 : powf(2.0f, 10.0f*(t-1.0f)); }
float fade_logarithmic(float t) { return t <= 0 ? 0 : logf(1.0f + 9.0f*t) / logf(10.0f); }
float fade_scurve(float t) { return t*t*t*(t*(t*6.0f-15.0f)+10.0f); }
float fade_sine(float t) { return 0.5f - 0.5f * cosf(t * M_PI); }

/* ================================================================
 * VIDEO STUTTER LOOP
 * ================================================================ */

/* Repeat a video frame buffer N times.
 * in: single frame (w*h*4 RGBA)
 * out: n_repeat frames concatenated
 */
int wb_video_stutter(const uint8_t *frame, int w, int h,
                      uint8_t *out, int n_repeat) {
    int frame_size = w * h * 4;
    for (int i = 0; i < n_repeat; i++) {
        memcpy(out + i * frame_size, frame, frame_size);
    }
    return n_repeat;
}

/* Stutter loop plus: apply different effect per iteration */
int wb_video_stutter_plus(const uint8_t *frame, int w, int h,
                           uint8_t *out, int n_repeat,
                           float (*effect)(float, int, void*), void *ctx) {
    int frame_size = w * h * 4;
    /* First frame: original */
    memcpy(out, frame, frame_size);

    for (int i = 1; i < n_repeat; i++) {
        uint8_t *dst = out + i * frame_size;
        memcpy(dst, frame, frame_size);
        /* Apply per-iteration effect (e.g., gain boost) */
        for (int p = 0; p < w * h * 4; p++) {
            float val = (float)dst[p] / 255.0f;
            val = effect(val, i, ctx);
            if (val < 0) val = 0;
            if (val > 1) val = 1;
            dst[p] = (uint8_t)(val * 255.0f);
        }
    }
    return n_repeat;
}

/* Stutter effect: brightness boost per iteration */
float wb_stutter_brightness(float sample, int iteration, void *ctx) {
    float boost = *(float*)ctx;
    return sample * powf(boost, (float)iteration);
}

/* ================================================================
 * COOKIE CUTTER / MASK SHAPES
 * ================================================================ */

/* Shape types for cookie cutter */
enum {
    WB_MASK_CIRCLE = 0,
    WB_MASK_RECTANGLE,
    WB_MASK_TRIANGLE,
    WB_MASK_STAR,
    WB_MASK_HEART,
    WB_MASK_DIAMOND,
    WB_MASK_HEXAGON,
    WB_MASK_CROSS
};

/* Apply a cookie cutter mask to an RGBA frame.
 * Pixels outside the shape are set to transparent (alpha=0).
 * shape: one of WB_MASK_*
 * cx, cy: center (0..1 normalized)
 * size: radius/half-width (0..1 normalized)
 */
void wb_cookie_cutter(uint8_t *rgba, int w, int h,
                       int shape, float cx, float cy, float size) {
    if (!rgba || w <= 0 || h <= 0) return;
    int icx = (int)(cx * w);
    int icy = (int)(cy * h);
    int irad = (int)(size * (w < h ? w : h));

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx = x - icx;
            int dy = y - icy;
            float dist = sqrtf((float)(dx*dx + dy*dy));
            int inside = 0;

            switch (shape) {
                case WB_MASK_CIRCLE:
                    inside = (dist <= irad);
                    break;
                case WB_MASK_RECTANGLE:
                    inside = (abs(dx) <= irad && abs(dy) <= irad);
                    break;
                case WB_MASK_TRIANGLE: {
                    /* Equilateral triangle pointing up */
                    float nx = (float)dx / irad;
                    float ny = (float)dy / irad;
                    inside = (ny >= -0.5f && ny <= 1.0f &&
                              fabsf(nx) <= (1.0f - ny) * 0.866f);
                    break;
                }
                case WB_MASK_STAR: {
                    /* 5-pointed star */
                    float angle = atan2f((float)dy, (float)dx);
                    float r = dist / irad;
                    float star_r = 0.5f + 0.5f * cosf(5.0f * angle);
                    inside = (r <= star_r);
                    break;
                }
                case WB_MASK_HEART: {
                    float nx = (float)dx / irad;
                    float ny = -(float)dy / irad; /* flip Y */
                    float hx = nx * nx + ny * ny - 1.0f;
                    inside = (hx*hx*hx - nx*nx*ny*ny*ny <= 0);
                    break;
                }
                case WB_MASK_DIAMOND:
                    inside = (abs(dx) + abs(dy) <= irad);
                    break;
                case WB_MASK_HEXAGON: {
                    float nx = fabsf((float)dx) / irad;
                    float ny = fabsf((float)dy) / irad;
                    inside = (nx <= 1.0f && ny <= 1.0f &&
                              nx + ny * 0.577f <= 1.0f);
                    break;
                }
                case WB_MASK_CROSS: {
                    int arm = irad / 3;
                    inside = (abs(dx) <= arm && abs(dy) <= irad) ||
                              (abs(dx) <= irad && abs(dy) <= arm);
                    break;
                }
                default:
                    inside = 1;
                    break;
            }

            if (!inside) {
                rgba[(y * w + x) * 4 + 3] = 0; /* transparent */
            }
        }
    }
}

/* ================================================================
 * MIRROR / KALEIDOSCOPE
 * ================================================================ */

/* Mirror effect: split screen into mirrored quadrants */
void wb_mirror_quad(uint8_t *dst, const uint8_t *src, int w, int h) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    int hw = w / 2, hh = h / 2;

    /* Top-left: original quadrant */
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < hw; x++)
            memcpy(dst + ((y * w + x) * 4), src + ((y * w + x) * 4), 4);

    /* Top-right: horizontal mirror */
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < hw; x++)
            memcpy(dst + ((y * w + (w - 1 - x)) * 4),
                   src + ((y * w + x) * 4), 4);

    /* Bottom-left: vertical mirror */
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < hw; x++)
            memcpy(dst + (((h - 1 - y) * w + x) * 4),
                   src + ((y * w + x) * 4), 4);

    /* Bottom-right: both mirrors */
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < hw; x++)
            memcpy(dst + (((h - 1 - y) * w + (w - 1 - x)) * 4),
                   src + ((y * w + x) * 4), 4);
}

/* Kaleidoscope: radial symmetry with N segments */
void wb_kaleidoscope(uint8_t *dst, const uint8_t *src, int w, int h, int segments) {
    if (!dst || !src || w <= 0 || h <= 0 || segments < 2) return;
    float cx = w * 0.5f, cy = h * 0.5f;
    float seg_angle = 2.0f * M_PI / segments;
    float max_r = sqrtf(cx*cx + cy*cy);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = x - cx, dy = y - cy;
            float r = sqrtf(dx*dx + dy*dy);
            float angle = atan2f(dy, dx);

            /* Map to first segment */
            float seg = floorf((angle + M_PI) / seg_angle);
            float local_angle = angle - seg * seg_angle;
            if (local_angle > seg_angle * 0.5f)
                local_angle = seg_angle - local_angle;

            /* Source coordinates */
            int sx = (int)(cx + r * cosf(local_angle));
            int sy = (int)(cy + r * sinf(local_angle));
            if (sx < 0) sx = 0; if (sx >= w) sx = w - 1;
            if (sy < 0) sy = 0; if (sy >= h) sy = h - 1;

            memcpy(dst + (y * w + x) * 4, src + (sy * w + sx) * 4, 4);
        }
    }
}

/* ================================================================
 * GEOMETRIC WARPS
 * ================================================================ */

/* Swirl: rotate pixels around center by angle proportional to distance */
void wb_swirl(uint8_t *dst, const uint8_t *src, int w, int h,
              float angle_deg, float radius) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    float cx = w * 0.5f, cy = h * 0.5f;
    float max_r = radius * (w < h ? w : h);
    float angle_rad = angle_deg * M_PI / 180.0f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = x - cx, dy = y - cy;
            float r = sqrtf(dx*dx + dy*dy);
            if (r < max_r) {
                float factor = 1.0f - r / max_r;
                float rot = angle_rad * factor * factor;
                float cosr = cosf(rot), sinr = sinf(rot);
                int sx = (int)(cx + dx * cosr - dy * sinr);
                int sy = (int)(cy + dx * sinr + dy * cosr);
                if (sx < 0) sx = 0; if (sx >= w) sx = w - 1;
                if (sy < 0) sy = 0; if (sy >= h) sy = h - 1;
                memcpy(dst + (y * w + x) * 4, src + (sy * w + sx) * 4, 4);
            } else {
                memcpy(dst + (y * w + x) * 4, src + (y * w + x) * 4, 4);
            }
        }
    }
}

/* Spherize: bulge or pinch effect */
void wb_spherize(uint8_t *dst, const uint8_t *src, int w, int h,
                 float strength, float radius) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    float cx = w * 0.5f, cy = h * 0.5f;
    float max_r = radius * (w < h ? w : h);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = x - cx, dy = y - cy;
            float r = sqrtf(dx*dx + dy*dy);
            if (r < max_r) {
                float factor = r / max_r;
                /* Bulge: strength > 0, Pinch: strength < 0 */
                float new_r = r * (1.0f + strength * (1.0f - factor * factor));
                float scale = (r > 0.001f) ? new_r / r : 1.0f;
                int sx = (int)(cx + dx * scale);
                int sy = (int)(cy + dy * scale);
                if (sx < 0) sx = 0; if (sx >= w) sx = w - 1;
                if (sy < 0) sy = 0; if (sy >= h) sy = h - 1;
                memcpy(dst + (y * w + x) * 4, src + (sy * w + sx) * 4, 4);
            } else {
                memcpy(dst + (y * w + x) * 4, src + (y * w + x) * 4, 4);
            }
        }
    }
}

/* Wave displacement: sin/cos pixel displacement */
void wb_wave_displace(uint8_t *dst, const uint8_t *src, int w, int h,
                      float amp_x, float freq_x, float phase_x,
                      float amp_y, float freq_y, float phase_y) {
    if (!dst || !src || w <= 0 || h <= 0) return;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = amp_x * sinf((float)x / w * freq_x * 2.0f * M_PI + phase_x);
            float dy = amp_y * sinf((float)y / h * freq_y * 2.0f * M_PI + phase_y);
            int sx = (int)(x + dx);
            int sy = (int)(y + dy);
            if (sx < 0) sx = 0; if (sx >= w) sx = w - 1;
            if (sy < 0) sy = 0; if (sy >= h) sy = h - 1;
            memcpy(dst + (y * w + x) * 4, src + (sy * w + sx) * 4, 4);
        }
    }
}

/* ================================================================
 * ZOOM PUNCH / IMPACT FRAME / FRAME FREEZE
 * ================================================================ */

/* Zoom punch: scale frame by factor (centered) */
void wb_zoom_punch(uint8_t *dst, const uint8_t *src, int w, int h, float scale) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    float cx = w * 0.5f, cy = h * 0.5f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = (int)(cx + (x - cx) / scale);
            int sy = (int)(cy + (y - cy) / scale);
            if (sx < 0) sx = 0; if (sx >= w) sx = w - 1;
            if (sy < 0) sy = 0; if (sy >= h) sy = h - 1;
            memcpy(dst + (y * w + x) * 4, src + (sy * w + sx) * 4, 4);
        }
    }
}

/* Impact frame: flash white or black for 1 frame */
void wb_impact_frame(uint8_t *frame, int w, int h, int white) {
    if (!frame || w <= 0 || h <= 0) return;
    uint8_t val = white ? 255 : 0;
    for (int i = 0; i < w * h; i++) {
        frame[i * 4 + 0] = val;
        frame[i * 4 + 1] = val;
        frame[i * 4 + 2] = val;
        frame[i * 4 + 3] = 255;
    }
}

/* ================================================================
 * SCRAMBLE / RANDOM CHOP
 * ================================================================ */

/* Scramble: divide frame into blocks and shuffle them */
void wb_scramble(uint8_t *dst, const uint8_t *src, int w, int h, int seed, int block_size) {
    if (!dst || !src || w <= 0 || h <= 0 || block_size < 1) return;
    int cols = w / block_size;
    int rows = h / block_size;
    int n_blocks = cols * rows;

    /* Generate permutation */
    srand(seed);
    int *perm = (int *)malloc(n_blocks * sizeof(int));
    if (!perm) return;
    for (int i = 0; i < n_blocks; i++) perm[i] = i;
    for (int i = n_blocks - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
    }

    /* Copy blocks in permuted order */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int dst_idx = r * cols + c;
            int src_idx = perm[dst_idx];
            int src_r = src_idx / cols;
            int src_c = src_idx % cols;

            for (int by = 0; by < block_size; by++) {
                for (int bx = 0; bx < block_size; bx++) {
                    int sx = src_c * block_size + bx;
                    int sy = src_r * block_size + by;
                    int dx = c * block_size + bx;
                    int dy = r * block_size + by;
                    if (sx < w && sy < h && dx < w && dy < h) {
                        memcpy(dst + (dy * w + dx) * 4, src + (sy * w + sx) * 4, 4);
                    }
                }
            }
        }
    }
    free(perm);
}

/* ================================================================
 * STROBE / FLASH
 * ================================================================ */

/* Strobe: alternate between frame and solid color */
void wb_strobe(uint8_t *dst, const uint8_t *src, int w, int h,
               int frame_num, int strobe_interval, uint8_t r, uint8_t g, uint8_t b) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    if ((frame_num / strobe_interval) % 2 == 0) {
        memcpy(dst, src, w * h * 4);
    } else {
        for (int i = 0; i < w * h; i++) {
            dst[i * 4 + 0] = r;
            dst[i * 4 + 1] = g;
            dst[i * 4 + 2] = b;
            dst[i * 4 + 3] = 255;
        }
    }
}

/* ================================================================
 * CRT / SCANLINES
 * ================================================================ */

/* CRT effect: scanlines + barrel curvature + phosphor glow */
void wb_crt_effect(uint8_t *dst, const uint8_t *src, int w, int h,
                   float scanline_intensity, float curvature) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    float cx = w * 0.5f, cy = h * 0.5f;
    float max_r = sqrtf(cx*cx + cy*cy);

    memset(dst, 0, w * h * 4); /* black background */

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* Barrel distortion */
            float dx = (x - cx) / cx;
            float dy = (y - cy) / cy;
            float r2 = dx*dx + dy*dy;
            float distort = 1.0f + curvature * r2;
            int sx = (int)(cx + (x - cx) * distort);
            int sy = (int)(cy + (y - cy) * distort);

            if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                /* Copy pixel */
                for (int c = 0; c < 4; c++) {
                    dst[(y * w + x) * 4 + c] = src[(sy * w + sx) * 4 + c];
                }

                /* Scanline darkening */
                if (y % 2 == 0) {
                    float dark = 1.0f - scanline_intensity;
                    dst[(y * w + x) * 4 + 0] = (uint8_t)(dst[(y * w + x) * 4 + 0] * dark);
                    dst[(y * w + x) * 4 + 1] = (uint8_t)(dst[(y * w + x) * 4 + 1] * dark);
                    dst[(y * w + x) * 4 + 2] = (uint8_t)(dst[(y * w + x) * 4 + 2] * dark);
                }
            }
        }
    }
}

/* ================================================================
 * RECURSION (DROSTE EFFECT)
 * ================================================================ */

/* Recursion: place a scaled-down copy of the frame inside itself */
void wb_recursion(uint8_t *dst, const uint8_t *src, int w, int h,
                  float scale, float cx, float cy, int depth) {
    if (!dst || !src || w <= 0 || h <= 0 || depth <= 0) return;

    /* First copy original */
    memcpy(dst, src, w * h * 4);

    /* Place scaled copy at center */
    int sw = (int)(w * scale);
    int sh = (int)(h * scale);
    int sx0 = (int)(cx * w - sw * 0.5f);
    int sy0 = (int)(cy * h - sh * 0.5f);

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int dx = sx0 + x;
            int dy = sy0 + y;
            if (dx >= 0 && dx < w && dy >= 0 && dy < h) {
                int sx = (int)((float)x / sw * w);
                int sy = (int)((float)y / sh * h);
                memcpy(dst + (dy * w + dx) * 4, src + (sy * w + sx) * 4, 4);
            }
        }
    }
}

/* ================================================================
 * MIDI: STEP SEQUENCER
 * ================================================================ */

#define WB_STEP_MAX 64

typedef struct {
    int steps[WB_STEP_MAX];     /* 0 or 1 per step */
    int n_steps;
    int current_step;
    int running;
} wb_step_seq;

void wb_step_seq_init(wb_step_seq *seq, int n_steps) {
    if (!seq) return;
    memset(seq, 0, sizeof(*seq));
    seq->n_steps = n_steps < WB_STEP_MAX ? n_steps : WB_STEP_MAX;
}

void wb_step_seq_set(wb_step_seq *seq, int step, int value) {
    if (!seq || step < 0 || step >= seq->n_steps) return;
    seq->steps[step] = value ? 1 : 0;
}

int wb_step_seq_tick(wb_step_seq *seq) {
    if (!seq || !seq->running) return 0;
    int val = seq->steps[seq->current_step];
    seq->current_step = (seq->current_step + 1) % seq->n_steps;
    return val;
}

void wb_step_seq_start(wb_step_seq *seq) {
    if (seq) { seq->running = 1; seq->current_step = 0; }
}

void wb_step_seq_stop(wb_step_seq *seq) {
    if (seq) seq->running = 0;
}

/* ================================================================
 * MIDI: EUCLIDEAN SEQUENCER
 * ================================================================ */

/* Generate euclidean rhythm: distribute hits as evenly as possible */
void wb_euclidean_rhythm(int *pattern, int n_steps, int n_hits) {
    if (!pattern || n_steps <= 0 || n_hits <= 0) return;
    if (n_hits > n_steps) n_hits = n_steps;

    /* Bjorklund's algorithm (simplified) */
    memset(pattern, 0, n_steps * sizeof(int));

    /* Simple even distribution */
    for (int i = 0; i < n_hits; i++) {
        int pos = (int)((float)i / n_hits * n_steps);
        if (pos >= n_steps) pos = n_steps - 1;
        pattern[pos] = 1;
    }
}

/* ================================================================
 * MIDI: PROBABILITY TRIGGER
 * ================================================================ */

/* Trigger note with given probability (0..1) */
int wb_midi_probability(float prob) {
    return ((float)rand() / RAND_MAX) < prob;
}

/* Ratchet: repeat note N times at 1/N interval */
int wb_midi_ratchet(int note, int velocity, int count, int *out_velocities) {
    if (!out_velocities || count <= 0) return 0;
    float decay = 0.8f;
    for (int i = 0; i < count; i++) {
        out_velocities[i] = (int)(velocity * powf(decay, i));
    }
    return count;
}

/* ================================================================
 * MIDI: AFTERTOUCH
 * ================================================================ */

typedef struct {
    uint8_t channel_pressure;   /* Channel aftertouch (mono) */
    uint8_t poly_pressure[128]; /* Polyphonic aftertouch per note */
    float modulation_depth;     /* How much aftertouch modulates target */
} wb_midi_aftertouch;

void wb_midi_aftertouch_init(wb_midi_aftertouch *at) {
    if (!at) return;
    memset(at, 0, sizeof(*at));
    at->modulation_depth = 1.0f;
}

void wb_midi_aftertouch_set_channel(wb_midi_aftertouch *at, uint8_t pressure) {
    if (at) at->channel_pressure = pressure;
}

void wb_midi_aftertouch_set_poly(wb_midi_aftertouch *at, uint8_t note, uint8_t pressure) {
    if (at && note < 128) at->poly_pressure[note] = pressure;
}

/* Get modulation value from aftertouch (0..1) */
float wb_midi_aftertouch_mod(const wb_midi_aftertouch *at, uint8_t note) {
    if (!at) return 0.0f;
    /* Blend channel and poly */
    float ch = at->channel_pressure / 127.0f;
    float poly = (note < 128) ? at->poly_pressure[note] / 127.0f : 0.0f;
    return (ch > poly ? ch : poly) * at->modulation_depth;
}

/* ================================================================
 * AUTOMATION: MODES (Touch/Latch/Write/Trim)
 * ================================================================ */

enum {
    WB_AUTOMATION_READ = 0,
    WB_AUTOMATION_TOUCH,
    WB_AUTOMATION_LATCH,
    WB_AUTOMATION_WRITE,
    WB_AUTOMATION_TRIM
};

typedef struct {
    int mode;
    float *keyframes;   /* time,value pairs: [t0,v0,t1,v1,...] */
    int n_points;
    int capacity;
    float current_value;
    float touch_start_value;
    float trim_offset;
    int touching;
} wb_automation_track;

void wb_automation_init(wb_automation_track *track, int capacity) {
    if (!track) return;
    memset(track, 0, sizeof(*track));
    track->keyframes = (float *)calloc(capacity * 2, sizeof(float));
    track->capacity = capacity;
    track->mode = WB_AUTOMATION_READ;
}

void wb_automation_set_mode(wb_automation_track *track, int mode) {
    if (!track) return;
    track->mode = mode;
    if (mode == WB_AUTOMATION_TRIM)
        track->trim_offset = 0.0f;
}

/* Add keyframe */
void wb_automation_add_keyframe(wb_automation_track *track, float time, float value) {
    if (!track || track->n_points >= track->capacity) return;
    /* Insert sorted by time */
    int idx = track->n_points;
    for (int i = track->n_points - 1; i >= 0; i--) {
        if (track->keyframes[i * 2] > time) {
            memmove(&track->keyframes[(i+1)*2], &track->keyframes[i*2],
                    (track->n_points - i) * 2 * sizeof(float));
            idx = i;
        } else break;
    }
    track->keyframes[idx * 2] = time;
    track->keyframes[idx * 2 + 1] = value;
    track->n_points++;
}

/* Evaluate automation at given time with interpolation */
float wb_automation_eval(const wb_automation_track *track, float time, int interp_type) {
    if (!track || track->n_points == 0) return 0.0f;

    /* Find surrounding keyframes */
    int i0 = 0, i1 = track->n_points - 1;
    for (int i = 0; i < track->n_points - 1; i++) {
        if (track->keyframes[i*2] <= time && track->keyframes[(i+1)*2] >= time) {
            i0 = i; i1 = i + 1; break;
        }
    }

    float t0 = track->keyframes[i0*2];
    float v0 = track->keyframes[i0*2+1];
    float t1 = track->keyframes[i1*2];
    float v1 = track->keyframes[i1*2+1];

    /* Interpolation factor */
    float t = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;
    if (t < 0) t = 0; if (t > 1) t = 1;

    float it = kf_interpolate(t, interp_type, 0.25f, 0.75f, 0.0f);
    return v0 + (v1 - v0) * it;
}

/* ================================================================
 * AUTOMATION: SNAPSHOT / MORPH
 * ================================================================ */

#define WB_SNAPSHOT_MAX_PARAMS 32

typedef struct {
    float params[WB_SNAPSHOT_MAX_PARAMS];
    char name[32];
} wb_snapshot;

typedef struct {
    wb_snapshot *snapshots;
    int n_snapshots;
    int capacity;
} wb_snapshot_bank;

void wb_snapshots_init(wb_snapshot_bank *bank, int capacity) {
    if (!bank) return;
    bank->snapshots = (wb_snapshot *)calloc(capacity, sizeof(wb_snapshot));
    bank->capacity = capacity;
    bank->n_snapshots = 0;
}

int wb_snapshots_save(wb_snapshot_bank *bank, const float *params, int n_params, const char *name) {
    if (!bank || bank->n_snapshots >= bank->capacity) return -1;
    wb_snapshot *s = &bank->snapshots[bank->n_snapshots];
    int n = n_params < WB_SNAPSHOT_MAX_PARAMS ? n_params : WB_SNAPSHOT_MAX_PARAMS;
    memcpy(s->params, params, n * sizeof(float));
    strncpy(s->name, name, 31);
    s->name[31] = '\0';
    return bank->n_snapshots++;
}

/* Morph between two snapshots */
void wb_snapshots_morph(const wb_snapshot_bank *bank, int idx_a, int idx_b,
                        float t, float *out, int n_params) {
    if (!bank || !out) return;
    if (idx_a < 0 || idx_a >= bank->n_snapshots) return;
    if (idx_b < 0 || idx_b >= bank->n_snapshots) return;

    const wb_snapshot *a = &bank->snapshots[idx_a];
    const wb_snapshot *b = &bank->snapshots[idx_b];
    int n = n_params < WB_SNAPSHOT_MAX_PARAMS ? n_params : WB_SNAPSHOT_MAX_PARAMS;

    float st = kf_scurve(t);
    for (int i = 0; i < n; i++) {
        out[i] = a->params[i] + (b->params[i] - a->params[i]) * st;
    }
}

/* ================================================================
 * PICTURE-IN-PICTURE
 * ================================================================ */

/* Overlay a small frame onto a larger frame at given position */
void wb_pip_overlay(uint8_t *dst, int dst_w, int dst_h,
                    const uint8_t *src, int src_w, int src_h,
                    int pos_x, int pos_y, float scale) {
    if (!dst || !src) return;
    int sw = (int)(src_w * scale);
    int sh = (int)(src_h * scale);

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int dx = pos_x + x;
            int dy = pos_y + y;
            if (dx < 0 || dx >= dst_w || dy < 0 || dy >= dst_h) continue;

            int sx = (int)((float)x / sw * src_w);
            int sy = (int)((float)y / sh * src_h);
            if (sx >= src_w || sy >= src_h) continue;

            const uint8_t *sp = src + (sy * src_w + sx) * 4;
            uint8_t *dp = dst + (dy * dst_w + dx) * 4;

            /* Alpha blend */
            float alpha = sp[3] / 255.0f;
            for (int c = 0; c < 3; c++) {
                dp[c] = (uint8_t)(sp[c] * alpha + dp[c] * (1.0f - alpha));
            }
            dp[3] = 255;
        }
    }
}

/* ================================================================
 * KEN BURNS EFFECT (pan/zoom across still image)
 * ================================================================ */

/* Evaluate Ken Burns transform at time t (0..1) */
void wb_ken_burns(uint8_t *dst, const uint8_t *src, int w, int h,
                  float t,
                  float start_x, float start_y, float start_scale,
                  float end_x, float end_y, float end_scale) {
    if (!dst || !src || w <= 0 || h <= 0) return;

    float st = kf_ease_inout(t);
    float cx = start_x + (end_x - start_x) * st;
    float cy = start_y + (end_y - start_y) * st;
    float scale = start_scale + (end_scale - start_scale) * st;

    int sw = (int)(w / scale);
    int sh = (int)(h / scale);
    int ox = (int)(cx * w - sw * 0.5f);
    int oy = (int)(cy * h - sh * 0.5f);

    memset(dst, 0, w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = ox + (int)((float)x / w * sw);
            int sy = oy + (int)((float)y / h * sh);
            if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                memcpy(dst + (y * w + x) * 4, src + (sy * w + sx) * 4, 4);
            }
        }
    }
}
