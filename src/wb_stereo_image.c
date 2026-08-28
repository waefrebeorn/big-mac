/* wb_stereo_image.c — stereo imaging / mid-side processing.
 *
 * R077: Control stereo width, mid-side balance.
 *
 * Algorithm:
 *   M = (L + R) / 2     (mid/center)
 *   S = (L - R) / 2     (side/stereo)
 *   L' = (M * mid_gain + S * side_gain)
 *   R' = (M * mid_gain - S * side_gain)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    float    mid_gain;       /* Center channel gain */
    float    side_gain;      /* Stereo channel gain */
    float    width;          /* 0=mono, 1=normal, 2=wide */
    float    mono_below;     /* Hz, collapse to mono below this freq */
    float    hp_state[4];    /* Filter for mono bass */
    float    hp_b0, hp_b1, hp_b2, hp_a1, hp_a2;
    uint32_t sr;
} wb_stereo_image_inst;

void *wb_stereo_image_create(uint32_t sr) {
    wb_stereo_image_inst *si = (wb_stereo_image_inst *)calloc(1, sizeof(*si));
    if (!si) return NULL;
    si->sr = sr;
    si->mid_gain = 1.0f;
    si->side_gain = 1.0f;
    si->width = 1.0f;
    si->mono_below = 120.0f;

    /* Highpass for bass mono */
    float omega = 2.0f * 3.14159265f * si->mono_below / (float)sr;
    float cos_o = cosf(omega);
    float sin_o = sinf(omega);
    float Q = 0.707f;
    float alpha = sin_o / (2.0f * Q);
    float a0 = 1.0f + alpha;
    si->hp_b0 = (1.0f + cos_o) / (2.0f * a0);
    si->hp_b1 = -(1.0f + cos_o) / a0;
    si->hp_b2 = (1.0f + cos_o) / (2.0f * a0);
    si->hp_a1 = (-2.0f * cos_o) / a0;
    si->hp_a2 = (1.0f - alpha) / a0;

    return si;
}

void wb_stereo_image_destroy(void *inst) { free(inst); }

void wb_stereo_image_set(void *inst, int param, float v) {
    wb_stereo_image_inst *si = (wb_stereo_image_inst *)inst;
    if (!si) return;
    switch (param) {
    case 0: /* width */
        si->width = v < 0 ? 0 : (v > 2 ? 2 : v);
        si->side_gain = si->width;
        break;
    case 1: /* mid gain */
        si->mid_gain = v < 0 ? 0 : (v > 2 ? 2 : v);
        break;
    case 2: /* mono below */
        si->mono_below = v > 20 ? v : 20;
        break;
    default: break;
    }
}

/* Process stereo block. */
void wb_stereo_image_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_stereo_image_inst *si = (wb_stereo_image_inst *)inst;
    if (!si) return;

    for (uint32_t i = 0; i < n; i++) {
        float l = L[i];
        float r = R[i];

        /* Mid-side encode */
        float mid = (l + r) * 0.5f;
        float side = (l - r) * 0.5f;

        /* Apply gains */
        float mid_out = mid * si->mid_gain;
        float side_out = side * si->side_gain;

        /* Bass mono (collapse side below threshold) */
        if (si->mono_below > 0) {
            float hp_side = si->hp_b0 * side + si->hp_b1 * si->hp_state[0] +
                            si->hp_b2 * si->hp_state[1] -
                            si->hp_a1 * si->hp_state[2] -
                            si->hp_a2 * si->hp_state[3];
            si->hp_state[1] = si->hp_state[0];
            si->hp_state[0] = side;
            si->hp_state[3] = si->hp_state[2];
            si->hp_state[2] = hp_side;
            side_out = hp_side * si->side_gain;
        }

        /* Decode */
        L[i] = mid_out + side_out;
        R[i] = mid_out - side_out;
    }
}
