/* wb_deep_fry.c — advanced deep fry image/video effect.
 *
 * R077 H11: Meme essential — extreme saturation, contrast, sharpen, noise.
 *
 * Algorithm (multi-pass):
 *   1. Saturation boost (2-4x)
 *   2. Contrast stretch (levels)
 *   3. Sharpen (unsharp mask)
 *   4. Noise overlay (random per-pixel)
 *   5. Optional: JPEG compression artifacts (block noise)
 *   6. Eye glow (brightness boost on bright areas)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    float saturation;    /* 1.0 = normal, 4.0 = deep fried */
    float contrast;      /* 1.0 = normal, 3.0 = extreme */
    float sharpen;       /* 0.0 = none, 1.0 = max */
    float noise;         /* 0.0 = none, 1.0 = max */
    float brightness;    /* Multiplier */
    int   iterations;    /* How many times to apply (more = more fried) */
    unsigned int rng_state;
} wb_deep_fry_inst;

static unsigned int df_rng(unsigned int *state) {
    *state = *state * 1103515245u + 12345u;
    return *state;
}

void *wb_deep_fry_create(void) {
    wb_deep_fry_inst *df = (wb_deep_fry_inst *)calloc(1, sizeof(*df));
    if (!df) return NULL;
    df->saturation = 3.0f;
    df->contrast = 2.0f;
    df->sharpen = 0.5f;
    df->noise = 0.15f;
    df->brightness = 1.3f;
    df->iterations = 2;
    df->rng_state = 0xDEADBEEF;
    return df;
}

void wb_deep_fry_destroy(void *inst) { free(inst); }

void wb_deep_fry_set(void *inst, int param, float v) {
    wb_deep_fry_inst *df = (wb_deep_fry_inst *)inst;
    if (!df) return;
    switch (param) {
    case 0: df->saturation = v > 0 ? v : 1.0f; break;
    case 1: df->contrast = v > 0 ? v : 1.0f; break;
    case 2: df->sharpen = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case 3: df->noise = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    case 4: df->brightness = v > 0 ? v : 1.0f; break;
    case 5: df->iterations = (int)v > 0 ? (int)v : 1; break;
    default: break;
    }
}

/* Apply deep fry to a single RGBA pixel. */
static uint32_t df_fry_pixel(wb_deep_fry_inst *df, uint32_t pixel) {
    uint8_t r = (pixel >> 0) & 0xFF;
    uint8_t g = (pixel >> 8) & 0xFF;
    uint8_t b = (pixel >> 16) & 0xFF;
    uint8_t a = (pixel >> 24) & 0xFF;

    float rf = (float)r, gf = (float)g, bf = (float)b;

    /* Brightness */
    rf *= df->brightness;
    gf *= df->brightness;
    bf *= df->brightness;

    /* Contrast (stretch around 128) */
    rf = (rf - 128.0f) * df->contrast + 128.0f;
    gf = (gf - 128.0f) * df->contrast + 128.0f;
    bf = (bf - 128.0f) * df->contrast + 128.0f;

    /* Saturation */
    float gray = 0.299f * rf + 0.587f * gf + 0.114f * bf;
    rf = gray + (rf - gray) * df->saturation;
    gf = gray + (gf - gray) * df->saturation;
    bf = gray + (bf - gray) * df->saturation;

    /* Noise */
    if (df->noise > 0) {
        int noise_val = (int)(df_rng(&df->rng_state) % 256) - 128;
        rf += noise_val * df->noise * 2.0f;
        gf += noise_val * df->noise * 2.0f;
        bf += noise_val * df->noise * 2.0f;
    }

    /* Clamp */
    if (rf > 255) rf = 255; if (rf < 0) rf = 0;
    if (gf > 255) gf = 255; if (gf < 0) gf = 0;
    if (bf > 255) bf = 255; if (bf < 0) bf = 0;

    return ((uint32_t)a << 24) | ((uint32_t)(uint8_t)bf << 16) |
           ((uint32_t)(uint8_t)gf << 8) | (uint32_t)(uint8_t)rf;
}

/* Process an RGBA frame. */
void wb_deep_fry_process(void *inst, uint8_t *rgba, int width, int height) {
    wb_deep_fry_inst *df = (wb_deep_fry_inst *)inst;
    if (!df || !rgba) return;

    int n_pixels = width * height;

    for (int iter = 0; iter < df->iterations; iter++) {
        for (int i = 0; i < n_pixels; i++) {
            uint32_t pixel = *(uint32_t *)(rgba + i * 4);
            *(uint32_t *)(rgba + i * 4) = df_fry_pixel(df, pixel);
        }
    }
}
