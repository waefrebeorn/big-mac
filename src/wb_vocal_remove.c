/* wb_vocal_remove.c — vocal removal via center channel extraction.
 *
 * R077: Remove vocals from stereo tracks.
 *
 * Algorithm (Center Cut):
 *   Mid = (L + R) / 2
 *   Side = (L - R) / 2
 *   Vocals are typically centered → in Mid
 *   Output = Side only (or attenuate Mid)
 *
 * Advanced: frequency-dependent center cut
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    float    attenuation_db;  /* How much to attenuate center (0=no change, -60=full removal) */
    float    freq_low;        /* Low freq for band-split removal */
    float    freq_high;       /* High freq for band-split removal */
    float    hp_state[4];
    float    lp_state[4];
} wb_vocal_remove_inst;

void *wb_vocal_remove_create(void) {
    wb_vocal_remove_inst *vr = (wb_vocal_remove_inst *)calloc(1, sizeof(*vr));
    if (!vr) return NULL;
    vr->attenuation_db = -20.0f;  /* Default: reduce center by 20dB */
    vr->freq_low = 200.0f;
    vr->freq_high = 4000.0f;
    return vr;
}

void wb_vocal_remove_destroy(void *inst) { free(inst); }

void wb_vocal_remove_set(void *inst, int param, float v) {
    wb_vocal_remove_inst *vr = (wb_vocal_remove_inst *)inst;
    if (!vr) return;
    switch (param) {
    case 0: vr->attenuation_db = v > 0 ? 0 : (v < -60 ? -60 : v); break;
    default: break;
    }
}

/* Simple vocal removal: attenuate center channel. */
void wb_vocal_remove_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_vocal_remove_inst *vr = (wb_vocal_remove_inst *)inst;
    if (!vr) return;

    float atten_linear = powf(10.0f, vr->attenuation_db / 20.0f);

    for (uint32_t i = 0; i < n; i++) {
        float l = L[i];
        float r = R[i];

        /* Mid-side */
        float mid = (l + r) * 0.5f;
        float side = (l - r) * 0.5f;

        /* Attenuate mid (vocals) */
        mid *= atten_linear;

        /* Decode back to L/R */
        L[i] = mid + side;
        R[i] = mid - side;
    }
}
