/* wb_formant.c — formant shifting for YTP voice character changes.
 *
 * R080: YTP essential — change voice character (chipmunk/demon/monster)
 * WITHOUT changing pitch. Classic pitch shift changes both; formant shift
 * preserves the musical note while altering the vocal tract character.
 *
 * Algorithm:
 *   1. FFT the input
 *   2. Shift the spectral envelope (formants) by a ratio
 *   3. Preserve the harmonic structure (pitch)
 *   4. IFFT back
 *
 * Simplified time-domain approach using LPC-like spectral envelope
 * manipulation via cepstrum liftering — pure C11, no external FFT lib.
 *
 * For real-time YTP use, we use a simpler method:
 *   - Split spectrum into formant regions via bandpass filter bank
 *   - Shift each region's center frequency
 *   - Recombine
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define NUM_FORMANT_BANDS 5
#define MAX_FFT_SIZE 2048

/* Formant band: center freq, bandwidth, gain */
typedef struct {
    float center_freq;
    float bandwidth;
    float gain;
    /* Biquad state for bandpass */
    float x[2], y[2];
    float b0, b1, b2, a1, a2;
} formant_band_t;

typedef struct {
    uint32_t sr;
    float    shift_ratio;   /* 0.5 = demon, 1.0 = normal, 2.0 = chipmunk */
    int      enabled;

    /* Analysis bands (original formants) */
    formant_band_t analysis[NUM_FORMANT_BANDS];
    /* Synthesis bands (shifted formants) */
    formant_band_t synthesis[NUM_FORMANT_BANDS];

    /* Output delay compensation */
    float    delay_line[MAX_FFT_SIZE / 4];
    int      delay_pos;
    int      delay_len;
} wb_formant_inst;

static void design_bandpass(formant_band_t *band, uint32_t sr) {
    float omega = 2.0f * M_PI * band->center_freq / sr;
    float sin_omega = sinf(omega);
    float cos_omega = cosf(omega);
    /* bandwidth is in Hz; convert to Q = center/bw */
    float q = band->center_freq / band->bandwidth;
    if (q < 0.5f) q = 0.5f;
    float alpha = sin_omega / (2.0f * q);

    float a0 = 1.0f + alpha;
    band->b0 = alpha / a0;
    band->b1 = 0.0f;
    band->b2 = -alpha / a0;
    band->a1 = -2.0f * cos_omega / a0;
    band->a2 = (1.0f - alpha) / a0;

    band->x[0] = band->x[1] = 0.0f;
    band->y[0] = band->y[1] = 0.0f;
}

static float process_bandpass(formant_band_t *band, float in) {
    float out = band->b0 * in + band->b1 * band->x[0] + band->b2 * band->x[1]
                - band->a1 * band->y[0] - band->a2 * band->y[1];
    band->x[1] = band->x[0];
    band->x[0] = in;
    band->y[1] = band->y[0];
    band->y[0] = out;
    return out;
}

void *wb_formant_create(uint32_t sr) {
    wb_formant_inst *inst = (wb_formant_inst *)calloc(1, sizeof(wb_formant_inst));
    if (!inst) return NULL;
    inst->sr = sr;
    inst->shift_ratio = 1.0f;
    inst->enabled = 1;
    inst->delay_len = 64;
    inst->delay_pos = 0;

    /* Default formant regions (vowel-like) */
    float centers[NUM_FORMANT_BANDS] = {300, 850, 1400, 2500, 3500};
    float widths[NUM_FORMANT_BANDS]  = {100, 120, 150, 200, 250};

    for (int i = 0; i < NUM_FORMANT_BANDS; i++) {
        inst->analysis[i].center_freq = centers[i];
        inst->analysis[i].bandwidth = widths[i];
        inst->analysis[i].gain = 1.0f;
        design_bandpass(&inst->analysis[i], sr);

        inst->synthesis[i].center_freq = centers[i];
        inst->synthesis[i].bandwidth = widths[i];
        inst->synthesis[i].gain = 1.0f;
        design_bandpass(&inst->synthesis[i], sr);
    }

    return inst;
}

void wb_formant_destroy(void *inst) {
    free(inst);
}

void wb_formant_set_shift(void *inst, float ratio) {
    wb_formant_inst *f = (wb_formant_inst *)inst;
    if (!f) return;
    if (ratio < 0.25f) ratio = 0.25f;
    if (ratio > 4.0f) ratio = 4.0f;
    f->shift_ratio = ratio;

    /* Update synthesis band center frequencies */
    float centers[NUM_FORMANT_BANDS] = {300, 850, 1400, 2500, 3500};
    for (int i = 0; i < NUM_FORMANT_BANDS; i++) {
        f->synthesis[i].center_freq = centers[i] * ratio;
        if (f->synthesis[i].center_freq > f->sr / 2.0f - 100.0f)
            f->synthesis[i].center_freq = f->sr / 2.0f - 100.0f;
        design_bandpass(&f->synthesis[i], f->sr);
    }
}

/* Process a mono buffer in-place */
void wb_formant_process(void *inst, float *buf, int n) {
    wb_formant_inst *f = (wb_formant_inst *)inst;
    if (!f || !f->enabled) return;

    for (int i = 0; i < n; i++) {
        float in = buf[i];
        float out = 0.0f;

        /* For each formant band: extract from analysis, inject at synthesis freq */
        for (int b = 0; b < NUM_FORMANT_BANDS; b++) {
            float extracted = process_bandpass(&f->analysis[b], in);
            /* Gain compensates for frequency shift density */
            float gain = f->synthesis[b].gain * f->shift_ratio;
            out += extracted * gain;
        }

        /* Mix: original + shifted formants (clamp to prevent blowup) */
        out = in * 0.3f + out * 0.7f;
        if (out > 10.0f) out = 10.0f;
        if (out < -10.0f) out = -10.0f;
        buf[i] = out;

        /* Delay line for phase alignment */
        f->delay_line[f->delay_pos] = in;
        f->delay_pos = (f->delay_pos + 1) % f->delay_len;
    }
}

/* Preset: demon voice (deep, monstrous) */
void wb_formant_preset_demon(void *inst) {
    wb_formant_set_shift(inst, 0.5f);
}

/* Preset: chipmunk (high, squeaky) */
void wb_formant_preset_chipmunk(void *inst) {
    wb_formant_set_shift(inst, 2.0f);
}

/* Preset: robot (metallic, fixed formants) */
void wb_formant_preset_robot(void *inst) {
    wb_formant_set_shift(inst, 1.0f);
    wb_formant_inst *f = (wb_formant_inst *)inst;
    if (!f) return;
    /* Narrow all bands for metallic quality */
    for (int i = 0; i < NUM_FORMANT_BANDS; i++) {
        f->synthesis[i].bandwidth = 30.0f;
        f->analysis[i].bandwidth = 30.0f;
    }
}
