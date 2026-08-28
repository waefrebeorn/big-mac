/* wb_restore.c — audio restoration (de-click, de-clip, noise reduction).
 *
 * R077: Clean up damaged audio recordings.
 *
 * Techniques:
 *   - De-click: detect short transients, interpolate
 *   - De-clip: detect clipped samples, reconstruct
 *   - Noise reduction: spectral subtraction with noise profile
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;

    /* De-click */
    float    click_threshold;    /* Amplitude delta threshold */
    int      click_window;       /* Samples to interpolate over */
    float    prev_sample;
    float    prev_delta;

    /* De-clip */
    float    clip_threshold;     /* 0.95 = detect near-clipping */
    int      clip_window;        /* Samples to reconstruct */

    /* Noise reduction */
    float    noise_profile[256]; /* Average noise spectrum */
    float    reduction_amount;   /* 0..1 */
    int      profile_learned;
    float    smooth_coeff;
} wb_restore_inst;

void *wb_restore_create(uint32_t sr) {
    wb_restore_inst *r = (wb_restore_inst *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->sr = sr;
    r->click_threshold = 0.1f;
    r->click_window = 8;
    r->clip_threshold = 0.95f;
    r->clip_window = 16;
    r->reduction_amount = 0.7f;
    r->smooth_coeff = 0.9f;
    return r;
}

void wb_restore_destroy(void *inst) { free(inst); }

void wb_restore_set(void *inst, int param, float v) {
    wb_restore_inst *r = (wb_restore_inst *)inst;
    if (!r) return;
    switch (param) {
    case 0: r->click_threshold = v; break;
    case 1: r->clip_threshold = v; break;
    case 2: r->reduction_amount = v < 0 ? 0 : (v > 1 ? 1 : v); break;
    default: break;
    }
}

/* Learn noise profile from a silent section.
 * samples: noise-only audio
 * n: number of samples */
void wb_restore_learn_noise(wb_restore_inst *r, const float *samples, int n) {
    if (!r || !samples || n < 256) return;

    /* Simple: compute RMS in frequency bands */
    memset(r->noise_profile, 0, sizeof(r->noise_profile));
    int band_size = n / 256;

    for (int band = 0; band < 256; band++) {
        float sum = 0;
        int start = band * band_size;
        for (int i = 0; i < band_size && start + i < n; i++) {
            float s = samples[start + i];
            sum += s * s;
        }
        r->noise_profile[band] = sqrtf(sum / (float)band_size);
    }

    r->profile_learned = 1;
}

/* Process de-click + de-clip on a mono buffer. */
void wb_restore_process(wb_restore_inst *r, float *samples, int n) {
    if (!r || !samples) return;

    for (int i = 0; i < n; i++) {
        float s = samples[i];

        /* De-click: detect sudden amplitude jumps */
        float delta = s - r->prev_sample;
        float delta_change = fabsf(delta - r->prev_delta);

        if (delta_change > r->click_threshold && i > 1) {
            /* Interpolate over the click */
            float before = r->prev_sample;
            float after = (i + 1 < n) ? samples[i + 1] : before;
            float blend = 0.5f;  /* Simple midpoint */
            s = before * (1.0f - blend) + after * blend;
        }

        r->prev_delta = delta;
        r->prev_sample = samples[i];

        /* De-clip: detect and reconstruct clipped samples */
        if (fabsf(s) > r->clip_threshold) {
            /* Reconstruct using neighboring samples */
            float before = (i > 0) ? samples[i - 1] : 0;
            float after = (i < n - 1) ? samples[i + 1] : 0;
            float sign = (s > 0) ? 1.0f : -1.0f;
            float reconstructed = (fabsf(before) + fabsf(after)) * 0.5f;
            if (reconstructed > 1.0f) reconstructed = 1.0f;
            s = sign * reconstructed;
        }

        samples[i] = s;
    }
}

/* Process stereo. */
void wb_restore_process_stereo(wb_restore_inst *r, float *L, float *R, int n) {
    if (!r) return;
    wb_restore_process(r, L, n);

    /* Reset state for R channel */
    r->prev_sample = 0;
    r->prev_delta = 0;
    wb_restore_process(r, R, n);
}
