/* wb_vfx.c — visual effects & compositing extensions (R077 Phase 2).
 *
 * Blend modes, 3D LUT loader (.cube), color correction,
 * transition effects, camera effects (shake, zoom, chromatic aberration),
 * and meme/YTP effects (deep fry, VHS, glitch, datamosh prep).
 *
 * Works with the existing wb_compositor.c node pipeline.
 * Pure C11, no third party.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ===================================================================
 * Blend Modes (W3C Compositing spec)
 * Each function takes base + blend channel (0..1) → result (0..1)
 * =================================================================== */

static inline float blend_multiply(float b, float s) { return b * s; }
static inline float blend_screen(float b, float s) { return b + s - b * s; }
static float blend_overlay(float s, float b) { (void)b;
    return b < 0.5f ? 2.0f * b * s : 1.0f - 2.0f * (1.0f - b) * (1.0f - s);
}
static inline float blend_darken(float b, float s) { return b < s ? b : s; }
static inline float blend_lighten(float b, float s) { return b > s ? b : s; }
static inline float blend_color_dodge(float b, float s) {
    return s >= 1.0f ? 1.0f : fminf(1.0f, b / (1.0f - s + 1e-6f));
}
static inline float blend_color_burn(float b, float s) {
    return s <= 0.0f ? 0.0f : fmaxf(0.0f, 1.0f - (1.0f - b) / (s + 1e-6f));
}
static inline float blend_hard_light(float b, float s) {
    return s < 0.5f ? 2.0f * b * s : 1.0f - 2.0f * (1.0f - b) * (1.0f - s);
}
static inline float blend_soft_light(float b, float s) {
    if (s <= 0.5f) {
        return b - (1.0f - 2.0f * s) * b * (1.0f - b);
    } else {
        float d = (b <= 0.25f) ? ((16.0f * b - 12.0f) * b + 4.0f) * b : sqrtf(b);
        return b + (2.0f * s - 1.0f) * (d - b);
    }
}
static inline float blend_difference(float b, float s) {
    return fabsf(b - s);
}
static inline float blend_exclusion(float b, float s) {
    return b + s - 2.0f * b * s;
}

typedef enum {
    WB_BLEND_NORMAL = 0,
    WB_BLEND_MULTIPLY,
    WB_BLEND_SCREEN,
    WB_BLEND_OVERLAY,
    WB_BLEND_DARKEN,
    WB_BLEND_LIGHTEN,
    WB_BLEND_COLOR_DODGE,
    WB_BLEND_COLOR_BURN,
    WB_BLEND_HARD_LIGHT,
    WB_BLEND_SOFT_LIGHT,
    WB_BLEND_DIFFERENCE,
    WB_BLEND_EXCLUSION,
    WB_BLEND_ADD,
    WB_BLEND_SUBTRACT,
    WB_BLEND_COUNT
} wb_blend_mode;

/* Apply blend mode to RGBA src over RGBA dst */
void wb_blend_pixels(uint8_t *dst, const uint8_t *src, int count, wb_blend_mode mode) {
    for (int i = 0; i < count; i++) {
        int idx = i * 4;
        float sb = src[idx+0] / 255.0f;
        float sg = src[idx+1] / 255.0f;
        float sbl = src[idx+2] / 255.0f;
        float sa = src[idx+3] / 255.0f;

        float db = dst[idx+0] / 255.0f;
        float dg = dst[idx+1] / 255.0f;
        float dbl = dst[idx+2] / 255.0f;
        float da = dst[idx+3] / 255.0f;

        float or_r, or_g, or_b;

        /* Pick blend function */
        switch (mode) {
        case WB_BLEND_MULTIPLY:    or_r=blend_multiply(db,sb); or_g=blend_multiply(dg,sg); or_b=blend_multiply(dbl,sbl); break;
        case WB_BLEND_SCREEN:      or_r=blend_screen(db,sb); or_g=blend_screen(dg,sg); or_b=blend_screen(dbl,sbl); break;
        case WB_BLEND_OVERLAY:     or_r=blend_overlay(db,sb); or_g=blend_overlay(dg,sg); or_b=blend_overlay(dbl,sbl); break;
        case WB_BLEND_DARKEN:      or_r=blend_darken(db,sb); or_g=blend_darken(dg,sg); or_b=blend_darken(dbl,sbl); break;
        case WB_BLEND_LIGHTEN:     or_r=blend_lighten(db,sb); or_g=blend_lighten(dg,sg); or_b=blend_lighten(dbl,sbl); break;
        case WB_BLEND_COLOR_DODGE: or_r=blend_color_dodge(db,sb); or_g=blend_color_dodge(dg,sg); or_b=blend_color_dodge(dbl,sbl); break;
        case WB_BLEND_COLOR_BURN:  or_r=blend_color_burn(db,sb); or_g=blend_color_burn(dg,sg); or_b=blend_color_burn(dbl,sbl); break;
        case WB_BLEND_HARD_LIGHT:  or_r=blend_hard_light(db,sb); or_g=blend_hard_light(dg,sg); or_b=blend_hard_light(dbl,sbl); break;
        case WB_BLEND_SOFT_LIGHT:  or_r=blend_soft_light(db,sb); or_g=blend_soft_light(dg,sg); or_b=blend_soft_light(dbl,sbl); break;
        case WB_BLEND_DIFFERENCE:  or_r=blend_difference(db,sb); or_g=blend_difference(dg,sg); or_b=blend_difference(dbl,sbl); break;
        case WB_BLEND_EXCLUSION:   or_r=blend_exclusion(db,sb); or_g=blend_exclusion(dg,sg); or_b=blend_exclusion(dbl,sbl); break;
        case WB_BLEND_ADD:         or_r=fminf(1,db+sb); or_g=fminf(1,dg+sg); or_b=fminf(1,dbl+sbl); break;
        case WB_BLEND_SUBTRACT:    or_r=fmaxf(0,db-sb); or_g=fmaxf(0,dg-sg); or_b=fmaxf(0,dbl-sbl); break;
        default:                   or_r=sb; or_g=sg; or_b=sbl; break;
        }

        /* Composite with alpha: out = blend * src_alpha + dst * (1 - src_alpha) */
        float out_a = sa + da * (1.0f - sa);
        if (out_a > 0.0f) {
            float r = (or_r * sa + db * da * (1.0f - sa)) / out_a;
            float g = (or_g * sa + dg * da * (1.0f - sa)) / out_a;
            float b = (or_b * sa + dbl * da * (1.0f - sa)) / out_a;
            dst[idx+0] = (uint8_t)(fminf(1,fmaxf(0,r)) * 255.0f + 0.5f);
            dst[idx+1] = (uint8_t)(fminf(1,fmaxf(0,g)) * 255.0f + 0.5f);
            dst[idx+2] = (uint8_t)(fminf(1,fmaxf(0,b)) * 255.0f + 0.5f);
        }
        dst[idx+3] = (uint8_t)(out_a * 255.0f + 0.5f);
    }
}

/* ===================================================================
 * 3D LUT (.cube) Loader
 * =================================================================== */

#define WB_LUT3D_SIZE 33
#define WB_LUT3D_MAX_PATH 256

typedef struct {
    float data[WB_LUT3D_SIZE][WB_LUT3D_SIZE][WB_LUT3D_SIZE][3]; /* RGB */
    int size;
    float domain_min[3];
    float domain_max[3];
} wb_lut3d;

/* Parse a .cube file. Returns 0 on success. */
int wb_lut3d_load(wb_lut3d *lut, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    memset(lut, 0, sizeof(*lut));
    lut->size = WB_LUT3D_SIZE;
    lut->domain_min[0] = lut->domain_min[1] = lut->domain_min[2] = 0.0f;
    lut->domain_max[0] = lut->domain_max[1] = lut->domain_max[2] = 1.0f;

    char line[1024];
    int parsed_header = 0;
    int r_idx = 0, g_idx = 0, b_idx = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        /* Parse header */
        if (strncmp(line, "TITLE", 5) == 0) continue;
        if (strncmp(line, "LUT_3D_SIZE", 11) == 0) {
            int sz;
            if (sscanf(line + 11, "%d", &sz) == 1) {
                lut->size = sz < WB_LUT3D_SIZE ? sz : WB_LUT3D_SIZE;
            }
            parsed_header = 1;
            continue;
        }
        if (strncmp(line, "DOMAIN_MIN", 10) == 0) {
            sscanf(line + 10, "%f %f %f", &lut->domain_min[0], &lut->domain_min[1], &lut->domain_min[2]);
            continue;
        }
        if (strncmp(line, "DOMAIN_MAX", 10) == 0) {
            sscanf(line + 10, "%f %f %f", &lut->domain_max[0], &lut->domain_max[1], &lut->domain_max[2]);
            continue;
        }

        /* Parse data line: R G B float values */
        float r, g, b;
        if (sscanf(line, "%f %f %f", &r, &g, &b) == 3) {
            if (r_idx < WB_LUT3D_SIZE && g_idx < WB_LUT3D_SIZE && b_idx < WB_LUT3D_SIZE) {
                lut->data[r_idx][g_idx][b_idx][0] = r;
                lut->data[r_idx][g_idx][b_idx][1] = g;
                lut->data[r_idx][g_idx][b_idx][2] = b;
            }
            b_idx++;
            if (b_idx >= lut->size) { b_idx = 0; g_idx++; }
            if (g_idx >= lut->size) { g_idx = 0; r_idx++; }
        }
    }
    fclose(f);
    return parsed_header ? 0 : -1;
}

/* Apply 3D LUT to RGBA buffer (trilinear interpolation) */
void wb_lut3d_apply(const wb_lut3d *lut, uint8_t *rgba, int count) {
    int n = lut->size;
    float scale = (float)(n - 1);

    for (int i = 0; i < count; i++) {
        int idx = i * 4;
        /* Normalize to 0..1, scale to LUT range */
        float r = rgba[idx+0] / 255.0f * scale;
        float g = rgba[idx+1] / 255.0f * scale;
        float b = rgba[idx+2] / 255.0f * scale;

        /* Clamp */
        if (r < 0) r = 0; if (r > scale) r = scale;
        if (g < 0) g = 0; if (g > scale) g = scale;
        if (b < 0) b = 0; if (b > scale) b = scale;

        /* Integer indices */
        int r0 = (int)r, g0 = (int)g, b0 = (int)b;
        int r1 = r0 + 1 < n ? r0 + 1 : r0;
        int g1 = g0 + 1 < n ? g0 + 1 : g0;
        int b1 = b0 + 1 < n ? b0 + 1 : b0;

        /* Fractional parts */
        float fr = r - r0, fg = g - g0, fb = b - b0;

        /* Trilinear interpolation */
        float out_r = 0, out_g = 0, out_b = 0;
        for (int c = 0; c < 3; c++) {
            float v000 = lut->data[r0][g0][b0][c];
            float v100 = lut->data[r1][g0][b0][c];
            float v010 = lut->data[r0][g1][b0][c];
            float v110 = lut->data[r1][g1][b0][c];
            float v001 = lut->data[r0][g0][b1][c];
            float v101 = lut->data[r1][g0][b1][c];
            float v011 = lut->data[r0][g1][b1][c];
            float v111 = lut->data[r1][g1][b1][c];

            float v00 = v000 * (1-fr) + v100 * fr;
            float v10 = v010 * (1-fr) + v110 * fr;
            float v01 = v001 * (1-fr) + v101 * fr;
            float v11 = v011 * (1-fr) + v111 * fr;

            float v0 = v00 * (1-fg) + v10 * fg;
            float v1 = v01 * (1-fg) + v11 * fg;

            float v = v0 * (1-fb) + v1 * fb;

            if (c == 0) out_r = v;
            else if (c == 1) out_g = v;
            else out_b = v;
        }

        rgba[idx+0] = (uint8_t)(fminf(1,fmaxf(0,out_r)) * 255.0f + 0.5f);
        rgba[idx+1] = (uint8_t)(fminf(1,fmaxf(0,out_g)) * 255.0f + 0.5f);
        rgba[idx+2] = (uint8_t)(fminf(1,fmaxf(0,out_b)) * 255.0f + 0.5f);
    }
}

/* ===================================================================
 * Color Correction (lift/gamma/gain + saturation + contrast)
 * =================================================================== */

typedef struct {
    float lift;     /* shadow offset (-1..1) */
    float gamma;    /* midtone power (0.1..10, 1.0 = identity) */
    float gain;     /* highlight multiplier (0..10) */
    float contrast; /* contrast amount (0..2, 1.0 = identity) */
    float saturation; /* saturation multiplier (0..10, 1.0 = identity) */
    float temperature; /* warm/cool shift (-1..1) */
    float hue;      /* hue rotation (radians) */
} wb_color_params;

void wb_color_correct(uint8_t *rgba, int count, const wb_color_params *p) {
    for (int i = 0; i < count; i++) {
        int idx = i * 4;
        float r = rgba[idx+0] / 255.0f;
        float g = rgba[idx+1] / 255.0f;
        float b = rgba[idx+2] / 255.0f;

        /* Lift (shadow offset) */
        r += p->lift;
        g += p->lift;
        b += p->lift;

        /* Clamp negatives */
        r = fmaxf(0, r);
        g = fmaxf(0, g);
        b = fmaxf(0, b);

        /* Gamma (midtone power). gamma > 1 = darker, gamma < 1 = brighter */
        if (p->gamma != 1.0f) {
            r = powf(r, p->gamma);
            g = powf(g, p->gamma);
            b = powf(b, p->gamma);
        }

        /* Gain (highlight multiply) */
        r *= p->gain;
        g *= p->gain;
        b *= p->gain;

        /* Contrast (pivot around 0.5) */
        if (p->contrast != 1.0f) {
            r = (r - 0.5f) * p->contrast + 0.5f;
            g = (g - 0.5f) * p->contrast + 0.5f;
            b = (b - 0.5f) * p->contrast + 0.5f;
        }

        /* Temperature (warm = +R, -B; cool = -R, +B) */
        r += p->temperature * 0.1f;
        b -= p->temperature * 0.1f;

        /* Hue rotation */
        if (p->hue != 0.0f) {
            float cos_h = cosf(p->hue);
            float sin_h = sinf(p->hue);
            float rr = r, gg = g, bb = b;
            r = rr * (0.213f + cos_h * 0.787f - sin_h * 0.213f) +
                gg * (0.715f - cos_h * 0.715f - sin_h * 0.715f) +
                bb * (0.072f - cos_h * 0.072f + sin_h * 0.928f);
            g = rr * (0.213f - cos_h * 0.213f + sin_h * 0.143f) +
                gg * (0.715f + cos_h * 0.285f + sin_h * 0.140f) +
                bb * (0.072f - cos_h * 0.072f - sin_h * 0.283f);
            b = rr * (0.213f - cos_h * 0.213f - sin_h * 0.787f) +
                gg * (0.715f - cos_h * 0.715f + sin_h * 0.715f) +
                bb * (0.072f + cos_h * 0.928f + sin_h * 0.072f);
        }

        /* Saturation */
        if (p->saturation != 1.0f) {
            float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            r = luma + (r - luma) * p->saturation;
            g = luma + (g - luma) * p->saturation;
            b = luma + (b - luma) * p->saturation;
        }

        /* Clamp and write */
        rgba[idx+0] = (uint8_t)(fminf(1,fmaxf(0,r)) * 255.0f + 0.5f);
        rgba[idx+1] = (uint8_t)(fminf(1,fmaxf(0,g)) * 255.0f + 0.5f);
        rgba[idx+2] = (uint8_t)(fminf(1,fmaxf(0,b)) * 255.0f + 0.5f);
    }
}

/* ===================================================================
 * Transition Effects
 * =================================================================== */

/* Cross dissolve: blend A→B by t (0..1) */
void wb_transition_dissolve(uint8_t *out, const uint8_t *a, const uint8_t *b,
                              int count, float t) {
    float inv = 1.0f - t;
    for (int i = 0; i < count; i++) {
        int idx = i * 4;
        out[idx+0] = (uint8_t)(a[idx+0] * inv + b[idx+0] * t);
        out[idx+1] = (uint8_t)(a[idx+1] * inv + b[idx+1] * t);
        out[idx+2] = (uint8_t)(a[idx+2] * inv + b[idx+2] * t);
        out[idx+3] = (uint8_t)(a[idx+3] * inv + b[idx+3] * t);
    }
}

/* Wipe: reveal B from left to right by t */
void wb_transition_wipe(uint8_t *out, const uint8_t *a, const uint8_t *b,
                          int width, int height, float t) {
    int cutoff = (int)(width * t);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            if (x < cutoff) {
                out[idx+0] = b[idx+0];
                out[idx+1] = b[idx+1];
                out[idx+2] = b[idx+2];
                out[idx+3] = b[idx+3];
            } else {
                out[idx+0] = a[idx+0];
                out[idx+1] = a[idx+1];
                out[idx+2] = a[idx+2];
                out[idx+3] = a[idx+3];
            }
        }
    }
}

/* Flash: white flash between A and B (t=0.5 = full white) */
void wb_transition_flash(uint8_t *out, const uint8_t *a, const uint8_t *b,
                           int count, float t) {
    /* Flash intensity: peaks at t=0.5, zero at t=0 and t=1 */
    float flash = 1.0f - fabsf(t - 0.5f) * 2.0f;  /* 0..1..0 */
    float inv = 1.0f - t;

    for (int i = 0; i < count; i++) {
        int idx = i * 4;
        float base_r = a[idx+0] * inv + b[idx+0] * t;
        float base_g = a[idx+1] * inv + b[idx+1] * t;
        float base_b = a[idx+2] * inv + b[idx+2] * t;
        out[idx+0] = (uint8_t)(base_r * (1-flash) + 255.0f * flash);
        out[idx+1] = (uint8_t)(base_g * (1-flash) + 255.0f * flash);
        out[idx+2] = (uint8_t)(base_b * (1-flash) + 255.0f * flash);
        out[idx+3] = 255;
    }
}

/* ===================================================================
 * Camera Effects
 * =================================================================== */

/* Camera shake: random offset per call. Intensity decays. */
typedef struct {
    float intensity;    /* current shake amount in pixels */
    float decay;        /* decay rate per second */
    float seed;         /* random seed accumulator */
} wb_camera_shake;

void wb_camera_shake_init(wb_camera_shake *s) {
    s->intensity = 0;
    s->decay = 5.0f;
    s->seed = 12345.0f;
}

void wb_camera_shake_trigger(wb_camera_shake *s, float amount) {
    s->intensity = amount;
}

void wb_camera_shake_update(wb_camera_shake *s, float dt) {
    s->intensity *= expf(-s->decay * dt);
    s->seed += dt * 1000.0f;
}

void wb_camera_shake_offset(wb_camera_shake *s, float *ox, float *oy) {
    if (s->intensity < 0.1f) { *ox = 0; *oy = 0; return; }
    /* Simple LCG random */
    s->seed = fmodf(s->seed * 1103515245.0f + 12345.0f, 2147483648.0f);
    float rx = (s->seed / 2147483648.0f - 0.5f) * 2.0f;
    s->seed = fmodf(s->seed * 1103515245.0f + 12345.0f, 2147483648.0f);
    float ry = (s->seed / 2147483648.0f - 0.5f) * 2.0f;
    *ox = rx * s->intensity;
    *oy = ry * s->intensity;
}

/* Chromatic aberration: offset R and B channels radially */
void wb_chromatic_aberration(uint8_t *rgba, int width, int height, float amount) {
    uint8_t *temp = malloc(width * height * 4);
    if (!temp) return;
    memcpy(temp, rgba, width * height * 4);

    float cx = width * 0.5f;
    float cy = height * 0.5f;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            float dx = (x - cx) / cx;
            float dy = (y - cy) / cy;
            float dist = sqrtf(dx*dx + dy*dy);

            int offset = (int)(dist * amount);
            if (offset < 1) offset = 1;

            /* R shifts outward */
            int rx = x + (dx > 0 ? offset : -offset);
            int ry = y + (dy > 0 ? offset : -offset);
            if (rx < 0) rx = 0; if (rx >= width) rx = width - 1;
            if (ry < 0) ry = 0; if (ry >= height) ry = height - 1;

            /* B shifts inward */
            int bx = x - (dx > 0 ? offset : -offset);
            int by = y - (dy > 0 ? offset : -offset);
            if (bx < 0) bx = 0; if (bx >= width) bx = width - 1;
            if (by < 0) by = 0; if (by >= height) by = height - 1;

            rgba[idx+0] = temp[(ry * width + rx) * 4 + 0];  /* R from offset */
            rgba[idx+1] = temp[idx+1];  /* G stays */
            rgba[idx+2] = temp[(by * width + bx) * 4 + 2];  /* B from opposite */
        }
    }
    free(temp);
}

/* ===================================================================
 * Meme / YTP Effects
 * =================================================================== */

/* Deep fry: extreme contrast + saturation + sharpening */
void wb_effect_deep_fry(uint8_t *rgba, int width, int height, float intensity) {
    /* Step 1: boost contrast and saturation */
    wb_color_params p = {0};
    p.contrast = 1.0f + intensity * 2.0f;
    p.saturation = 1.0f + intensity * 3.0f;
    p.gamma = 0.8f;
    p.gain = 1.2f;
    wb_color_correct(rgba, width * height, &p);

    /* Step 2: sharpen (unsharp mask: original + amount*(original-blurred)) */
    uint8_t *blurred = malloc(width * height * 4);
    if (!blurred) return;
    memcpy(blurred, rgba, width * height * 4);

    /* Simple box blur 3x3 */
    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            for (int c = 0; c < 3; c++) {
                int sum = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        sum += rgba[((y+dy)*width + (x+dx))*4 + c];
                blurred[(y*width + x)*4 + c] = (uint8_t)(sum / 9);
            }
        }
    }

    /* Apply unsharp mask */
    float amount = intensity * 2.0f;
    for (int i = 0; i < width * height * 4; i += 4) {
        for (int c = 0; c < 3; c++) {
            int val = rgba[i+c] + (int)((rgba[i+c] - blurred[i+c]) * amount);
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            rgba[i+c] = (uint8_t)val;
        }
    }

    free(blurred);
}

/* VHS effect: tracking lines + noise + color bleed */
void wb_effect_vhs(uint8_t *rgba, int width, int height, float time, float intensity) {
    float tracking_y = fmodf(time * 120.0f, (float)height);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;

            /* Tracking line: bright horizontal band */
            float track_dist = fabsf(y - tracking_y);
            if (track_dist < 3.0f) {
                float flash = 1.0f - track_dist / 3.0f;
                rgba[idx+0] = (uint8_t)fminf(255, rgba[idx+0] + flash * 80 * intensity);
                rgba[idx+1] = (uint8_t)fminf(255, rgba[idx+1] + flash * 80 * intensity);
                rgba[idx+2] = (uint8_t)fminf(255, rgba[idx+2] + flash * 80 * intensity);
            }

            /* Noise */
            float noise = ((float)rand() / RAND_MAX - 0.5f) * 40.0f * intensity;
            int r = (int)rgba[idx+0] + (int)noise;
            int g = (int)rgba[idx+1] + (int)noise;
            int b = (int)rgba[idx+2] + (int)noise;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            rgba[idx+0] = (uint8_t)r;
            rgba[idx+1] = (uint8_t)g;
            rgba[idx+2] = (uint8_t)b;

            /* Scanlines */
            if (y % 2 == 0) {
                rgba[idx+0] = (uint8_t)(rgba[idx+0] * 0.9f);
                rgba[idx+1] = (uint8_t)(rgba[idx+1] * 0.9f);
                rgba[idx+2] = (uint8_t)(rgba[idx+2] * 0.9f);
            }

            /* Color bleed: shift R channel slightly left */
            if (x > 2) {
                rgba[idx+0] = (uint8_t)(rgba[idx+0] * 0.7f + rgba[idx-4] * 0.3f);
            }
        }
    }
}

/* RGB glitch: separate R/G/B channels with horizontal offset */
void wb_effect_rgb_glitch(uint8_t *rgba, int width, int height, float intensity) {
    uint8_t *temp = malloc(width * height * 4);
    if (!temp) return;
    memcpy(temp, rgba, width * height * 4);

    int max_offset = (int)(20 * intensity);

    for (int y = 0; y < height; y++) {
        /* Per-row random offset */
        int offset = (rand() % (max_offset * 2 + 1)) - max_offset;
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            int sx = x + offset;
            if (sx < 0) sx = 0;
            if (sx >= width) sx = width - 1;

            rgba[idx+0] = temp[(y * width + sx) * 4 + 0];  /* R from offset */
            rgba[idx+1] = temp[idx+1];                      /* G stays */
            rgba[idx+2] = temp[(y * width + (x - offset < 0 ? 0 : (x - offset >= width ? width-1 : x-offset))) * 4 + 2];  /* B opposite */
        }
    }
    free(temp);
}

/* Posterize: reduce color depth */
void wb_effect_posterize(uint8_t *rgba, int count, int levels) {
    if (levels < 2) levels = 2;
    if (levels > 255) levels = 255;
    float step = 255.0f / (float)(levels - 1);
    for (int i = 0; i < count * 4; i += 4) {
        rgba[i+0] = (uint8_t)(roundf(rgba[i+0] / step) * step);
        rgba[i+1] = (uint8_t)(roundf(rgba[i+1] / step) * step);
        rgba[i+2] = (uint8_t)(roundf(rgba[i+2] / step) * step);
    }
}

/* Vignette: darken edges */
void wb_effect_vignette(uint8_t *rgba, int width, int height, float strength) {
    float cx = width * 0.5f;
    float cy = height * 0.5f;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            float dx = (x - cx) / cx;
            float dy = (y - cy) / cy;
            float dist = sqrtf(dx*dx + dy*dy);

            float factor = 1.0f - strength * dist * dist;
            if (factor < 0) factor = 0;

            rgba[idx+0] = (uint8_t)(rgba[idx+0] * factor);
            rgba[idx+1] = (uint8_t)(rgba[idx+1] * factor);
            rgba[idx+2] = (uint8_t)(rgba[idx+2] * factor);
        }
    }
}
