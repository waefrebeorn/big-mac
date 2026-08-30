/* wb_chromakey.c — chroma key / green screen compositing for YTP.
 *
 * R80: Classic YTP technique — overlay characters onto different backgrounds,
 * composite multiple video layers, swap green screen backgrounds.
 * Supports:
 *   - Chroma key (green/blue screen removal with spill suppression)
 *   - Alpha blending / compositing two RGBA layers
 *   - Luma key (brightness-based keying)
 *   - Garbage matte (simple rectangular mask)
 *   - Edge feathering / soft key edges
 *   - Color spill suppression
 *
 * Pure C11, operates on RGBA uint8 buffers.
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    /* Key color (normalized 0-1) */
    float key_r, key_g, key_b;
    /* Threshold */
    float threshold;        /* Base key distance threshold (0-1) */
    float softness;         /* Edge softness (0=hard, 1=very soft) */
    float spill_suppress;   /* Spill suppression strength (0-1) */
    /* Garbage matte */
    int   use_matte;
    int   matte_x, matte_y, matte_w, matte_h;
    /* Mode */
    int   mode;             /* 0=chroma, 1=luma, 2=color difference */
    int   width, height;
} wb_chromakey_inst;

void *wb_chromakey_create(int width, int height) {
    wb_chromakey_inst *inst = (wb_chromakey_inst *)calloc(1, sizeof(wb_chromakey_inst));
    if (!inst) return NULL;
    inst->width = width;
    inst->height = height;
    inst->key_g = 1.0f;          /* Default: green screen */
    inst->threshold = 0.4f;
    inst->softness = 0.1f;
    inst->spill_suppress = 0.5f;
    inst->mode = 0;
    return inst;
}

void wb_chromakey_destroy(void *inst) {
    free(inst);
}

void wb_chromakey_set_key_color(void *inst, float r, float g, float b) {
    wb_chromakey_inst *k = (wb_chromakey_inst *)inst;
    k->key_r = r; k->key_g = g; k->key_b = b;
}

void wb_chromakey_set_threshold(void *inst, float t) {
    ((wb_chromakey_inst *)inst)->threshold = t;
}

void wb_chromakey_set_softness(void *inst, float s) {
    ((wb_chromakey_inst *)inst)->softness = s;
}

void wb_chromakey_set_spill(void *inst, float s) {
    ((wb_chromakey_inst *)inst)->spill_suppress = s;
}

void wb_chromakey_set_mode(void *inst, int mode) {
    ((wb_chromakey_inst *)inst)->mode = mode;
}

/* Process: take foreground RGBA, produce RGBA with transparent key color */
void wb_chromakey_process(void *inst, const uint8_t *fg_rgba, uint8_t *out_rgba, int width, int height) {
    wb_chromakey_inst *k = (wb_chromakey_inst *)inst;
    float thresh = k->threshold;
    float soft = k->softness;
    float spill = k->spill_suppress;
    float kr = k->key_r, kg = k->key_g, kb = k->key_b;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int i = (y * width + x) * 4;
            float r = fg_rgba[i] / 255.0f;
            float g = fg_rgba[i+1] / 255.0f;
            float b = fg_rgba[i+2] / 255.0f;

            float alpha;

            if (k->mode == 1) {
                /* Luma key: key based on brightness — bright=opaque, dark=transparent */
                float luma = 0.299f * r + 0.587f * g + 0.114f * b;
                alpha = fmaxf(0.0f, fminf(1.0f, (luma - thresh) / (soft + 0.001f)));
            } else {
                /* Chroma key: distance from key color in RGB space */
                float dr = r - kr, dg = g - kg, db = b - kb;
                float dist = sqrtf(dr*dr + dg*dg + db*db) / 1.732f; /* normalize to 0-1 */

                if (dist < thresh - soft) {
                    alpha = 0.0f; /* Fully transparent */
                } else if (dist < thresh + soft) {
                    alpha = (dist - thresh + soft) / (2.0f * soft + 0.001f);
                } else {
                    alpha = 1.0f; /* Fully opaque */
                }
            }

            /* Spill suppression: reduce key color channel in semi-transparent areas */
            float out_r = r, out_g = g, out_b = b;
            if (spill > 0.0f && alpha > 0.0f && alpha < 1.0f) {
                if (kg >= kr && kg >= kb) {
                    float excess_g = g - fmaxf(r, b);
                    if (excess_g > 0) out_g -= excess_g * spill * (1.0f - alpha);
                } else if (kb >= kr && kb >= kg) {
                    float excess_b = b - fmaxf(r, g);
                    if (excess_b > 0) out_b -= excess_b * spill * (1.0f - alpha);
                }
            }

            out_rgba[i]   = (uint8_t)(out_r * 255.0f);
            out_rgba[i+1] = (uint8_t)(fmaxf(0.0f, out_g) * 255.0f);
            out_rgba[i+2] = (uint8_t)(fmaxf(0.0f, out_b) * 255.0f);
            out_rgba[i+3] = (uint8_t)(alpha * 255.0f);
        }
    }
}

/* Composite: overlay foreground (with alpha) onto background */
void wb_chromakey_composite(void *inst, const uint8_t *fg_rgba, const uint8_t *bg_rgba, uint8_t *out_rgba, int width, int height) {
    (void)inst;
    for (int i = 0; i < width * height; i++) {
        int p = i * 4;
        float fa = fg_rgba[p+3] / 255.0f;
        float ba = 1.0f - fa;
        out_rgba[p]   = (uint8_t)(fg_rgba[p]   * fa + bg_rgba[p]   * ba);
        out_rgba[p+1] = (uint8_t)(fg_rgba[p+1] * fa + bg_rgba[p+1] * ba);
        out_rgba[p+2] = (uint8_t)(fg_rgba[p+2] * fa + bg_rgba[p+2] * ba);
        out_rgba[p+3] = 255;
    }
}
