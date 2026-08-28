/* wb_chroma_key.c — green screen / chroma key compositing.
 *
 * Meme essential: replace green background with any image/video.
 *
 * Algorithm:
 *   1. Convert RGB to YUV/HSV for better color separation
 *   2. Compute distance from key color in chromaticity space
 *   3. Apply soft threshold with feathering for anti-aliasing
 *   4. Spill suppression (remove green tint on edges)
 *   5. Composite foreground over background using alpha
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint8_t r, g, b;
} rgb_t;

typedef struct {
    float key_r, key_g, key_b;    /* key color (default green) */
    float threshold;               /* similarity threshold (0..1) */
    float feather;                 /* edge softness */
    float spill_suppress;          /* green spill removal */
} wb_chroma_key_params;

typedef struct {
    wb_chroma_key_params params;
} wb_chroma_key_inst;

void *wb_chroma_key_create(void) {
    wb_chroma_key_inst *ck = (wb_chroma_key_inst *)calloc(1, sizeof(*ck));
    if (!ck) return NULL;
    /* Default: green screen */
    ck->params.key_r = 0.0f;
    ck->params.key_g = 1.0f;
    ck->params.key_b = 0.0f;
    ck->params.threshold = 0.4f;
    ck->params.feather = 0.1f;
    ck->params.spill_suppress = 0.5f;
    return ck;
}

void wb_chroma_key_destroy(void *inst) { free(inst); }

void wb_chroma_key_set(void *inst, int param, float v) {
    wb_chroma_key_inst *ck = (wb_chroma_key_inst *)inst;
    if (!ck) return;
    switch (param) {
    case 0: /* key color R */
        ck->params.key_r = v < 0 ? 0 : (v > 1 ? 1 : v);
        break;
    case 1: /* key color G */
        ck->params.key_g = v < 0 ? 0 : (v > 1 ? 1 : v);
        break;
    case 2: /* key color B */
        ck->params.key_b = v < 0 ? 0 : (v > 1 ? 1 : v);
        break;
    case 3: /* threshold */
        ck->params.threshold = v < 0.05f ? 0.05f : (v > 1 ? 1 : v);
        break;
    case 4: /* feather */
        ck->params.feather = v < 0 ? 0 : (v > 0.5f ? 0.5f : v);
        break;
    case 5: /* spill suppression */
        ck->params.spill_suppress = v < 0 ? 0 : (v > 1 ? 1 : v);
        break;
    default: break;
    }
}

/* Compute alpha (transparency) for a pixel based on distance from key color.
 * Returns 0.0 (fully keyed/transparent) to 1.0 (fully opaque). */
static float chroma_alpha(uint8_t r, uint8_t g, uint8_t b,
                           const wb_chroma_key_params *p) {
    /* Normalize to 0..1 */
    float pr = (float)r / 255.0f;
    float pg = (float)g / 255.0f;
    float pb = (float)b / 255.0f;

    /* Convert to chromaticity (ignore luminance) */
    float sum = pr + pg + pb + 1e-6f;
    float cr = pr / sum;
    float cg = pg / sum;
    float cb = pb / sum;

    /* Key chromaticity */
    float ksum = p->key_r + p->key_g + p->key_b + 1e-6f;
    float kr = p->key_r / ksum;
    float kg = p->key_g / ksum;
    float kb = p->key_b / ksum;

    /* Distance in chromaticity space */
    float dist = sqrtf((cr - kr) * (cr - kr) +
                       (cg - kg) * (cg - kg) +
                       (cb - kb) * (cb - kb));

    /* Soft threshold with feathering */
    float alpha;
    if (dist < p->threshold - p->feather) {
        alpha = 0.0f;  /* fully keyed */
    } else if (dist > p->threshold + p->feather) {
        alpha = 1.0f;  /* fully opaque */
    } else {
        /* Linear interpolation in feather zone */
        alpha = (dist - (p->threshold - p->feather)) / (2.0f * p->feather);
    }

    return alpha;
}

/* Apply chroma key to a single pixel, composite over background. */
static inline void chroma_key_pixel(wb_chroma_key_inst *ck,
                                     uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                                     uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                                     uint8_t *out_r, uint8_t *out_g, uint8_t *out_b) {
    float alpha = chroma_alpha(fg_r, fg_g, fg_b, &ck->params);

    /* Spill suppression: reduce green channel near edges */
    if (alpha > 0.1f && alpha < 0.9f) {
        float spill = (1.0f - alpha) * ck->params.spill_suppress;
        float excess_g = (float)fg_g - ((float)fg_r + (float)fg_b) * 0.5f;
        if (excess_g > 0) {
            fg_r = (uint8_t)((float)fg_r + excess_g * spill * 0.3f);
            fg_b = (uint8_t)((float)fg_b + excess_g * spill * 0.3f);
        }
    }

    /* Composite: out = fg * alpha + bg * (1 - alpha) */
    *out_r = (uint8_t)((float)fg_r * alpha + (float)bg_r * (1.0f - alpha));
    *out_g = (uint8_t)((float)fg_g * alpha + (float)bg_g * (1.0f - alpha));
    *out_b = (uint8_t)((float)fg_b * alpha + (float)bg_b * (1.0f - alpha));
}

/* Process an RGBA frame (foreground) over a background.
 * fg: RGBA pixel data (4 bytes per pixel)
 * bg: RGB pixel data (3 bytes per pixel)
 * out: RGB output (3 bytes per pixel)
 * width, height: frame dimensions */
void wb_chroma_key_process(wb_chroma_key_inst *ck,
                            const uint8_t *fg_rgba,
                            const uint8_t *bg_rgb,
                            uint8_t *out_rgb,
                            int width, int height) {
    if (!ck || !fg_rgba || !bg_rgb || !out_rgb) return;

    int n_pixels = width * height;
    for (int i = 0; i < n_pixels; i++) {
        int fg_idx = i * 4;
        int bg_idx = i * 3;
        int out_idx = i * 3;

        chroma_key_pixel(ck,
                         fg_rgba[fg_idx], fg_rgba[fg_idx + 1], fg_rgba[fg_idx + 2],
                         bg_rgb[bg_idx], bg_rgb[bg_idx + 1], bg_rgb[bg_idx + 2],
                         &out_rgb[out_idx], &out_rgb[out_idx + 1], &out_rgb[out_idx + 2]);
    }
}

/* Simple version: just generate alpha mask from green screen.
 * Useful for testing or when background is separate. */
void wb_chroma_key_mask(wb_chroma_key_inst *ck,
                         const uint8_t *rgba,
                         uint8_t *alpha_out,
                         int width, int height) {
    if (!ck || !rgba || !alpha_out) return;

    int n_pixels = width * height;
    for (int i = 0; i < n_pixels; i++) {
        int idx = i * 4;
        float a = chroma_alpha(rgba[idx], rgba[idx + 1], rgba[idx + 2], &ck->params);
        alpha_out[i] = (uint8_t)(a * 255.0f);
    }
}
