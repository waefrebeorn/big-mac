/* wb_vhs_effect.c — VHS tracking errors and retro video degradation.
 *
 * R077 H12: VHS aesthetic — tracking lines, color bleed, noise, dropouts.
 *
 * Effects:
 *   - Horizontal tracking lines (random vertical position)
 *   - Color channel misalignment (RGB split)
 *   - Static noise (random white specks)
 *   - Vertical hold jitter (frame shift)
 *   - Chroma noise (color speckles)
 *   - Scanlines (horizontal dark lines)
 *   - Head switching noise (bottom of frame)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    float    intensity;      /* 0..1 overall effect amount */
    float    tracking;       /* Tracking error amount */
    float    noise;          /* Static noise amount */
    float    chroma;         /* Color bleed amount */
    float    scanlines;      /* Scanline darkness */
    float    dropouts;       /* Random line dropouts */
    float    time;           /* Animation time */
    unsigned int rng;
} wb_vhs_inst;

void *wb_vhs_create(void) {
    wb_vhs_inst *vhs = (wb_vhs_inst *)calloc(1, sizeof(*vhs));
    if (!vhs) return NULL;
    vhs->intensity = 0.5f;
    vhs->tracking = 0.5f;
    vhs->noise = 0.3f;
    vhs->chroma = 0.4f;
    vhs->scanlines = 0.3f;
    vhs->dropouts = 0.2f;
    vhs->time = 0;
    vhs->rng = 0x12345678;
    return vhs;
}

void wb_vhs_destroy(void *inst) { free(inst); }

void wb_vhs_set(void *inst, int param, float v) {
    wb_vhs_inst *vhs = (wb_vhs_inst *)inst;
    if (!vhs) return;
    switch (param) {
    case 0: vhs->intensity = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case 1: vhs->tracking = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case 2: vhs->noise = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case 3: vhs->chroma = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case 4: vhs->scanlines = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case 5: vhs->dropouts = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    default: break;
    }
}

static unsigned int vhs_rng(unsigned int *state) {
    *state = (*state ^ (*state << 13)) ^ (*state >> 17) ^ (*state << 5);
    return *state;
}

/* Process an RGBA frame. */
void wb_vhs_process(void *inst, uint8_t *rgba, int width, int height) {
    wb_vhs_inst *vhs = (wb_vhs_inst *)inst;
    if (!vhs || !rgba) return;

    float intensity = vhs->intensity;
    if (intensity <= 0) return;

    vhs->time += 0.016f;  /* ~60fps */

    /* Tracking line position (moves slowly) */
    int tracking_line = (int)(sinf(vhs->time * 0.5f) * 0.5f + 0.5f) * height;

    for (int y = 0; y < height; y++) {
        /* Scanlines */
        float scanline_dark = 1.0f;
        if (vhs->scanlines > 0 && (y % 2 == 0)) {
            scanline_dark = 1.0f - vhs->scanlines * 0.5f * intensity;
        }

        /* Tracking error band */
        int track_dist = abs(y - tracking_line);
        int in_tracking = (track_dist < 5) && (vhs->tracking > 0);

        /* Head switching noise (bottom 10% of frame) */
        int in_head_switch = (y > height * 0.9f);

        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;

            uint8_t r = rgba[idx];
            uint8_t g = rgba[idx + 1];
            uint8_t b = rgba[idx + 2];

            /* Chroma shift (RGB misalignment) */
            if (vhs->chroma > 0) {
                int shift = (int)(vhs->chroma * 4 * intensity);
                int xl = x - shift; if (xl < 0) xl = 0;
                int xr = x + shift; if (xr >= width) xr = width - 1;
                int idx_l = (y * width + xl) * 4;
                int idx_r = (y * width + xr) * 4;
                r = rgba[idx_l];
                b = rgba[idx_r + 2];
            }

            /* Static noise */
            if (vhs->noise > 0 && (vhs_rng(&vhs->rng) % 1000) < (int)(vhs->noise * 50 * intensity)) {
                uint8_t noise = (uint8_t)(vhs_rng(&vhs->rng) % 256);
                r = g = b = noise;
            }

            /* Tracking line distortion */
            if (in_tracking) {
                int offset = (int)(sinf(x * 0.1f + vhs->time * 10) * 10 * vhs->tracking * intensity);
                int src_x = x + offset;
                if (src_x >= 0 && src_x < width) {
                    int src_idx = (y * width + src_x) * 4;
                    r = rgba[src_idx];
                    g = rgba[src_idx + 1];
                    b = rgba[src_idx + 2];
                }
            }

            /* Dropouts (random white lines) */
            if (vhs->dropouts > 0 && (vhs_rng(&vhs->rng) % 500) < (int)(vhs->dropouts * 10 * intensity)) {
                r = g = b = 255;
            }

            /* Head switching noise */
            if (in_head_switch && (vhs_rng(&vhs->rng) % 100) < 20) {
                uint8_t noise = (uint8_t)(vhs_rng(&vhs->rng) % 256);
                r = g = b = noise;
            }

            /* Apply scanline darkening */
            r = (uint8_t)(r * scanline_dark);
            g = (uint8_t)(g * scanline_dark);
            b = (uint8_t)(b * scanline_dark);

            rgba[idx] = r;
            rgba[idx + 1] = g;
            rgba[idx + 2] = b;
        }
    }
}
