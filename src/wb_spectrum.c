/* wb_spectrum.c — audio spectrum analyzer / visualizer.
 *
 * R77: Real-time frequency spectrum for music video visualization.
 *
 * Algorithm:
 *   1. Windowed FFT (Hann window)
 *   2. Magnitude computation per bin
 *   3. Log-frequency scaling (perceptual)
 *   4. Smoothing (peak hold + decay)
 *
 * Outputs: magnitude[0..num_bars-1] normalized 0..1
 *
 * Pure C11. Uses wb_fft.c for FFT. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define SPECTRUM_MAX_BARS 64
#define SPECTRUM_FFT_SIZE 1024


typedef struct {
    uint32_t sr;
    int      fft_size;
    int      num_bars;
    float    window[SPECTRUM_FFT_SIZE];
    float    fft_input[SPECTRUM_FFT_SIZE];
    float    magnitudes[SPECTRUM_FFT_SIZE / 2];
    float    bars[SPECTRUM_MAX_BARS];
    float    smoothed[SPECTRUM_MAX_BARS];
    float    peak_hold[SPECTRUM_MAX_BARS];
    int      input_pos;
    float    smooth_coeff;
    float    decay_coeff;
} wb_spectrum_inst;


/* Forward declaration */
static void wb_spectrum_compute(wb_spectrum_inst *sp);
void *wb_spectrum_create(uint32_t sr) {
    wb_spectrum_inst *sp = (wb_spectrum_inst *)calloc(1, sizeof(*sp));
    if (!sp) return NULL;
    sp->sr = sr;
    sp->fft_size = SPECTRUM_FFT_SIZE;
    sp->num_bars = 32;  /* default */
    sp->input_pos = 0;
    sp->smooth_coeff = 0.3f;
    sp->decay_coeff = 0.95f;

    /* Hann window */
    for (int i = 0; i < sp->fft_size; i++) {
        sp->window[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * (float)i / (float)(sp->fft_size - 1)));
    }

    return sp;
}

void wb_spectrum_destroy(void *inst) {
    free(inst);
}

void wb_spectrum_set_bars(void *inst, int num_bars) {
    wb_spectrum_inst *sp = (wb_spectrum_inst *)inst;
    if (!sp) return;
    sp->num_bars = num_bars;
    if (sp->num_bars > SPECTRUM_MAX_BARS) sp->num_bars = SPECTRUM_MAX_BARS;
    if (sp->num_bars < 1) sp->num_bars = 1;
}

/* Feed audio samples. Call this every frame with audio buffer. */
void wb_spectrum_feed(wb_spectrum_inst *sp, const float *mono, int n) {
    if (!sp || !mono) return;

    for (int i = 0; i < n; i++) {
        sp->fft_input[sp->input_pos] = mono[i] * sp->window[sp->input_pos];
        sp->input_pos++;

        if (sp->input_pos >= sp->fft_size) {
            /* FFT buffer full — compute spectrum */
            wb_spectrum_compute(sp);
            sp->input_pos = 0;
        }
    }
}

/* Compute spectrum from current FFT buffer.
 * Simplified: uses magnitude of DFT bins (no FFT for portability).
 * For production, replace with wb_fft.c radix-2 FFT. */
static void wb_spectrum_compute(wb_spectrum_inst *sp) {
    int fft_size = sp->fft_size;
    int num_bins = fft_size / 2;

    /* Compute magnitude per frequency bin (simplified DFT) */
    for (int k = 0; k < num_bins; k++) {
        float re = 0, im = 0;
        for (int n = 0; n < fft_size; n++) {
            float angle = -2.0f * 3.14159265f * (float)k * (float)n / (float)fft_size;
            re += sp->fft_input[n] * cosf(angle);
            im += sp->fft_input[n] * sinf(angle);
        }
        sp->magnitudes[k] = sqrtf(re * re + im * im) / (float)fft_size;
    }

    /* Map to log-frequency bars */
    float max_val = 1e-10f;
    for (int i = 0; i < num_bins; i++) {
        if (sp->magnitudes[i] > max_val) max_val = sp->magnitudes[i];
    }

    for (int bar = 0; bar < sp->num_bars; bar++) {
        /* Log frequency mapping */
        float f_start = (float)bar / (float)sp->num_bars;
        float f_end = (float)(bar + 1) / (float)sp->num_bars;

        /* Convert to frequency bins (log scale) */
        float freq_start = powf(10.0f, f_start * 3.0f + 1.0f);  /* 10Hz to 10kHz */
        float freq_end = powf(10.0f, f_end * 3.0f + 1.0f);

        int bin_start = (int)(freq_start / ((float)sp->sr / (float)fft_size));
        int bin_end = (int)(freq_end / ((float)sp->sr / (float)fft_size));

        if (bin_start < 0) bin_start = 0;
        if (bin_end > num_bins) bin_end = num_bins;
        if (bin_end <= bin_start) bin_end = bin_start + 1;

        /* Average magnitude in this band */
        float sum = 0;
        for (int b = bin_start; b < bin_end; b++) {
            sum += sp->magnitudes[b];
        }
        float avg = sum / (float)(bin_end - bin_start);

        /* Normalize and smooth */
        float normalized = avg / max_val;
        sp->smoothed[bar] = sp->smooth_coeff * sp->smoothed[bar] +
                            (1.0f - sp->smooth_coeff) * normalized;

        /* Peak hold */
        if (sp->smoothed[bar] > sp->peak_hold[bar]) {
            sp->peak_hold[bar] = sp->smoothed[bar];
        } else {
            sp->peak_hold[bar] *= sp->decay_coeff;
        }

        sp->bars[bar] = sp->smoothed[bar];
    }
}

/* Get current bar magnitudes (0..1). */
const float* wb_spectrum_get_bars(wb_spectrum_inst *sp, int *out_num_bars) {
    if (!sp) return NULL;
    if (out_num_bars) *out_num_bars = sp->num_bars;
    return sp->bars;
}

/* Get peak hold values (0..1). */
const float* wb_spectrum_get_peaks(wb_spectrum_inst *sp) {
    return sp ? sp->peak_hold : NULL;
}
