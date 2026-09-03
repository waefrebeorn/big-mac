/* wb_chromakey_engine.c — C11 engine-level chroma key processing
 * Provides the functions that wb_video_fx_pro.c expects:
 *   wb_chromakey_create/destroy/process/set_key_color/set_threshold/set_softness
 *
 * Pure C11 pixel processing — no ffmpeg dependency.
 * Works in linear RGB space with distance-based keying.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    int w, h;
    float key_r, key_g, key_b;  /* Key color in 0..1 range */
    float threshold;             /* Distance threshold (0..1) */
    float softness;              /* Edge softness (0..1) */
    float spill;                 /* Spill suppression strength */
} wb_chromakey_inst;

void *wb_chromakey_create(int width, int height) {
    wb_chromakey_inst *k = (wb_chromakey_inst *)calloc(1, sizeof(wb_chromakey_inst));
    if (!k) return NULL;
    k->w = width;
    k->h = height;
    k->key_r = 0.0f;
    k->key_g = 1.0f;  /* Default green screen */
    k->key_b = 0.0f;
    k->threshold = 0.4f;
    k->softness = 0.1f;
    k->spill = 0.5f;
    return k;
}

void wb_chromakey_destroy(void *inst) {
    free(inst);
}

void wb_chromakey_set_key_color(void *inst, float r, float g, float b) {
    wb_chromakey_inst *k = (wb_chromakey_inst *)inst;
    if (!k) return;
    k->key_r = r;
    k->key_g = g;
    k->key_b = b;
}

void wb_chromakey_set_threshold(void *inst, float t) {
    wb_chromakey_inst *k = (wb_chromakey_inst *)inst;
    if (!k) return;
    k->threshold = t;
}

void wb_chromakey_set_softness(void *inst, float s) {
    wb_chromakey_inst *k = (wb_chromakey_inst *)inst;
    if (!k) return;
    k->softness = s;
}

void wb_chromakey_process(void *inst, const uint8_t *fg, uint8_t *out, int w, int h) {
    wb_chromakey_inst *k = (wb_chromakey_inst *)inst;
    if (!k || !fg || !out) return;

    float kr = k->key_r, kg = k->key_g, kb = k->key_b;
    float thresh = k->threshold;
    float soft = k->softness;
    float spill_str = k->spill;

    /* Convert key color to 0..255 */
    float key_r255 = kr * 255.0f;
    float key_g255 = kg * 255.0f;
    float key_b255 = kb * 255.0f;

    /* Precompute key color luminance for spill detection */
    float key_lum = 0.299f * kr + 0.587f * kg + 0.114f * kb;

    for (int i = 0; i < w * h; i++) {
        float r = fg[i*4+0];
        float g = fg[i*4+1];
        float b = fg[i*4+2];
        float a = fg[i*4+3];

        /* Distance from key color in RGB space */
        float dr = r - key_r255;
        float dg = g - key_g255;
        float db = b - key_b255;
        float dist = sqrtf(dr*dr + dg*dg + db*db) / 441.67f; /* Normalize to 0..1 */

        /* Compute alpha based on distance */
        float alpha;
        if (dist < thresh - soft) {
            alpha = 0.0f;  /* Fully transparent (keyed out) */
        } else if (dist < thresh + soft) {
            /* Soft edge: linear interpolation */
            alpha = (dist - (thresh - soft)) / (2.0f * soft);
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 1.0f) alpha = 1.0f;
        } else {
            alpha = 1.0f;  /* Fully opaque */
        }

        /* Spill suppression: reduce key color channel */
        float lum = 0.299f * r + 0.587f * g + 0.114f * b;
        float spill_amount = 0.0f;

        if (kg > kr && kg > kb) {
            /* Green screen spill */
            float excess_g = g - lum;
            if (excess_g > 0) {
                spill_amount = excess_g * spill_str * (1.0f - alpha);
                g -= spill_amount;
                if (g < 0) g = 0;
            }
        } else if (kb > kr && kb > kg) {
            /* Blue screen spill */
            float excess_b = b - lum;
            if (excess_b > 0) {
                spill_amount = excess_b * spill_str * (1.0f - alpha);
                b -= spill_amount;
                if (b < 0) b = 0;
            }
        }

        /* Premultiplied alpha output */
        float final_alpha = (alpha * a) / 255.0f;
        out[i*4+0] = (uint8_t)(r * final_alpha);
        out[i*4+1] = (uint8_t)(g * final_alpha);
        out[i*4+2] = (uint8_t)(b * final_alpha);
        out[i*4+3] = (uint8_t)(final_alpha * 255.0f);
    }
}
