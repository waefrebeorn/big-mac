/* wb_sonogram.c — real-time spectrogram + waveform display (iZotope RX style).
 *
 * R079: Advanced audio analysis / sonogram visualization.
 *
 * Algorithm:
 *   1. STFT with Hann window, 2048 frame, 512 hop
 *   2. FFT via wb_fft.c (radix-2 Cooley-Tukey)
 *   3. Magnitude → log scale → color map (black→blue→green→yellow→red)
 *   4. Time scrolls left→right; frequency on vertical axis (log scale)
 *   5. Waveform overlay at top ~15% of render buffer
 *   6. Peak / RMS / crest factor from time-domain signal
 *   7. Spectral centroid: weighted mean frequency from FFT bins
 *
 * Pure C11, zero third-party. Uses wb_fft.c for FFT. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus/wbus_sonogram.h"
#include "wbus/wbus_fft.h"

#define SG_FFT_SIZE  2048
#define SG_HOP_SIZE  512
#define SG_MAX_COLS  2048   /* max spectrogram time columns stored */

typedef struct wb_sonogram {
    uint32_t sr;
    int      width, height;
    int      fft_size;
    int      hop_size;

    /* FFT plan + working buffers */
    wb_fft_plan *fft_plan;
    double      *fft_re;
    double      *fft_im;
    double      *window;      /* Hann window coefficients */

    /* Input accumulation buffer (mono) */
    float   *input_buf;
    int      input_pos;
    int      input_cap;

    /* Spectrogram column history: each column is fft_size/2 magnitude values.
     * Stored as float[col][bin], row-major. We keep SG_MAX_COLS columns. */
    float   *columns;          /* SG_MAX_COLS x (fft_size/2) */
    int      col_count;        /* how many columns filled so far */
    int      col_write;        /* next write index (ring buffer) */

    /* Time-domain stats */
    float    peak;
    float    rms_accum;
    uint32_t rms_frames;

    /* Spectral centroid from most recent FFT frame */
    float    spectral_centroid;

    /* Waveform ring buffer for overlay (last width*2 samples) */
    float   *wave_buf;
    int      wave_pos;
    int      wave_cap;
} wb_sonogram;

/* ---- helpers ----------------------------------------------------------- */

static double hann(int n, int N) {
    return 0.5 * (1.0 - cos(2.0 * M_PI * (double)n / (double)(N - 1)));
}

/* Map normalized magnitude [0..1] to color.
 * Gradient: black → blue → green → yellow → red. */
static void mag_to_color(float mag, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (mag < 0.0f) mag = 0.0f;
    if (mag > 1.0f) mag = 1.0f;

    /* 4 segments: 0..0.25, 0.25..0.5, 0.5..0.75, 0.75..1.0 */
    if (mag < 0.25f) {
        /* black → blue */
        float t = mag / 0.25f;
        *r = 0;
        *g = 0;
        *b = (uint8_t)(t * 255.0f);
    } else if (mag < 0.5f) {
        /* blue → green */
        float t = (mag - 0.25f) / 0.25f;
        *r = 0;
        *g = (uint8_t)(t * 255.0f);
        *b = (uint8_t)((1.0f - t) * 255.0f);
    } else if (mag < 0.75f) {
        /* green → yellow */
        float t = (mag - 0.5f) / 0.25f;
        *r = (uint8_t)(t * 255.0f);
        *g = 255;
        *b = 0;
    } else {
        /* yellow → red */
        float t = (mag - 0.75f) / 0.25f;
        *r = 255;
        *g = (uint8_t)((1.0f - t) * 255.0f);
        *b = 0;
    }
}

/* ---- public API -------------------------------------------------------- */

wb_sonogram *wb_sonogram_create(uint32_t sr, int width, int height) {
    if (sr == 0 || width <= 0 || height <= 0) return NULL;
    if (width > SG_MAX_COLS) width = SG_MAX_COLS;

    wb_sonogram *sg = (wb_sonogram *)calloc(1, sizeof(*sg));
    if (!sg) return NULL;

    sg->sr = sr;
    sg->width = width;
    sg->height = height;
    sg->fft_size = SG_FFT_SIZE;
    sg->hop_size = SG_HOP_SIZE;

    /* FFT plan */
    sg->fft_plan = wb_fft_create(SG_FFT_SIZE);
    if (!sg->fft_plan) { free(sg); return NULL; }

    sg->fft_re = (double *)calloc(SG_FFT_SIZE, sizeof(double));
    sg->fft_im = (double *)calloc(SG_FFT_SIZE, sizeof(double));
    sg->window = (double *)calloc(SG_FFT_SIZE, sizeof(double));
    sg->input_buf = (float *)calloc(SG_FFT_SIZE, sizeof(float));
    sg->columns = (float *)calloc((size_t)SG_MAX_COLS * (SG_FFT_SIZE / 2), sizeof(float));
    sg->wave_cap = width * 4;
    sg->wave_buf = (float *)calloc(sg->wave_cap, sizeof(float));

    if (!sg->fft_re || !sg->fft_im || !sg->window || !sg->input_buf ||
        !sg->columns || !sg->wave_buf) {
        wb_fft_destroy(sg->fft_plan);
        free(sg->fft_re); free(sg->fft_im); free(sg->window);
        free(sg->input_buf); free(sg->columns); free(sg->wave_buf);
        free(sg);
        return NULL;
    }

    /* Precompute Hann window */
    for (int i = 0; i < SG_FFT_SIZE; i++)
        sg->window[i] = hann(i, SG_FFT_SIZE);

    sg->input_cap = SG_FFT_SIZE;
    sg->input_pos = 0;
    sg->col_count = 0;
    sg->col_write = 0;
    sg->peak = 0.0f;
    sg->rms_accum = 0.0f;
    sg->rms_frames = 0;
    sg->spectral_centroid = 0.0f;
    sg->wave_pos = 0;

    return sg;
}

void wb_sonogram_destroy(wb_sonogram *sg) {
    if (!sg) return;
    wb_fft_destroy(sg->fft_plan);
    free(sg->fft_re);
    free(sg->fft_im);
    free(sg->window);
    free(sg->input_buf);
    free(sg->columns);
    free(sg->wave_buf);
    free(sg);
}

/* Compute one FFT frame and store the magnitude column */
static void wb_sonogram_compute_frame(wb_sonogram *sg) {
    /* Window the input and convert to double */
    for (int i = 0; i < SG_FFT_SIZE; i++) {
        sg->fft_re[i] = (double)sg->input_buf[i] * sg->window[i];
        sg->fft_im[i] = 0.0;
    }

    /* Forward FFT */
    wb_fft_run(sg->fft_plan, sg->fft_re, sg->fft_im, 0);

    /* Compute magnitudes for bins 0..fft_size/2 and store as column */
    int num_bins = SG_FFT_SIZE / 2;
    float *col = sg->columns + (size_t)sg->col_write * num_bins;

    double mag_sum = 0.0;
    double weighted_freq_sum = 0.0;
    float max_mag = 0.0f;

    for (int k = 0; k < num_bins; k++) {
        double re = sg->fft_re[k];
        double im = sg->fft_im[k];
        float mag = (float)sqrt(re * re + im * im);
        col[k] = mag;
        if (mag > max_mag) max_mag = mag;

        double freq = (double)k * (double)sg->sr / (double)SG_FFT_SIZE;
        weighted_freq_sum += freq * (double)mag;
        mag_sum += (double)mag;
    }

    /* Spectral centroid */
    if (mag_sum > 1e-20)
        sg->spectral_centroid = (float)(weighted_freq_sum / mag_sum);
    else
        sg->spectral_centroid = 0.0f;

    /* Advance column ring buffer */
    sg->col_write = (sg->col_write + 1) % SG_MAX_COLS;
    if (sg->col_count < SG_MAX_COLS)
        sg->col_count++;
}

int wb_sonogram_process(wb_sonogram *sg, const float *audio, uint32_t frames) {
    if (!sg || !audio || frames == 0) return -1;

    for (uint32_t i = 0; i < frames; i++) {
        float sample = audio[i];

        /* Track peak and RMS */
        float abs_s = sample > 0.0f ? sample : -sample;
        if (abs_s > sg->peak) sg->peak = abs_s;
        sg->rms_accum += sample * sample;
        sg->rms_frames++;

        /* Store in waveform ring buffer */
        sg->wave_buf[sg->wave_pos] = sample;
        sg->wave_pos = (sg->wave_pos + 1) % sg->wave_cap;

        /* Accumulate into FFT input buffer */
        sg->input_buf[sg->input_pos++] = sample;

        /* When buffer full, compute FFT and shift by hop */
        if (sg->input_pos >= SG_FFT_SIZE) {
            wb_sonogram_compute_frame(sg);

            /* Shift buffer by hop size (overlap) */
            int remainder = SG_FFT_SIZE - SG_HOP_SIZE;
            memmove(sg->input_buf, sg->input_buf + SG_HOP_SIZE,
                    remainder * sizeof(float));
            sg->input_pos = remainder;
        }
    }

    return 0;
}

int wb_sonogram_render(wb_sonogram *sg, uint8_t *rgba_out, int width, int height) {
    if (!sg || !rgba_out || width <= 0 || height <= 0) return -1;

    /* Clamp to sonogram's internal width */
    if (width > sg->width) width = sg->width;
    if (height > sg->height) height = sg->height;

    int num_bins = SG_FFT_SIZE / 2;
    int wave_h = height / 6;  /* waveform overlay = top ~16% */
    if (wave_h < 4) wave_h = 4;
    int spec_h = height - wave_h;

    /* Clear to black */
    memset(rgba_out, 0, (size_t)width * height * 4);

    /* ---- Spectrogram body (bottom spec_h rows) ---- */
    /* Map columns to x-pixels. If we have fewer columns than width,
     * right-align (newest at right). If more, take the most recent `width`. */
    int cols_avail = sg->col_count < width ? sg->col_count : width;
    int col_start = sg->col_write - cols_avail;
    if (col_start < 0) col_start += SG_MAX_COLS;

    /* Find global max magnitude for normalization */
    float global_max = 1e-10f;
    for (int c = 0; c < cols_avail; c++) {
        int ci = (col_start + c) % SG_MAX_COLS;
        float *col = sg->columns + (size_t)ci * num_bins;
        for (int k = 0; k < num_bins; k++) {
            if (col[k] > global_max) global_max = col[k];
        }
    }

    for (int x = 0; x < width; x++) {
        /* Which column does this x map to? */
        int ci;
        if (x < cols_avail) {
            ci = (col_start + x) % SG_MAX_COLS;
        } else {
            /* No data for this column — leave black */
            continue;
        }

        float *col = sg->columns + (size_t)ci * num_bins;

        for (int y = 0; y < spec_h; y++) {
            /* Map y=0 (top of spectrogram) → high freq, y=spec_h-1 → low freq.
             * Use log scale: freq = sr/2 * exp(-y/scale) kind of mapping. */
            float frac = 1.0f - (float)y / (float)(spec_h - 1);  /* 0=bottom, 1=top */

            /* Log frequency mapping: map frac [0..1] to bin index.
             * Low freq at bottom (frac=0), high freq at top (frac=1). */
            float min_freq = 20.0f;
            float max_freq = (float)sg->sr / 2.0f;
            float freq = min_freq * powf(max_freq / min_freq, frac);

            /* Convert freq to bin */
            float bin_f = freq / ((float)sg->sr / (float)SG_FFT_SIZE);
            int bin_lo = (int)floorf(bin_f);
            int bin_hi = bin_lo + 1;
            float t = bin_f - (float)bin_lo;

            if (bin_lo < 0) bin_lo = 0;
            if (bin_hi >= num_bins) bin_hi = num_bins - 1;
            if (bin_lo >= num_bins) bin_lo = num_bins - 1;

            /* Linear interpolation between bins */
            float mag = col[bin_lo] * (1.0f - t) + col[bin_hi] * t;

            /* Normalize + apply log compression for visual dynamic range */
            float norm = mag / global_max;
            /* sqrt compression: brings up quiet details */
            norm = sqrtf(norm);

            uint8_t r, g, b;
            mag_to_color(norm, &r, &g, &b);

            /* Write pixel: y=0 in spectrogram region is at screen y=wave_h */
            int py = wave_h + y;
            int idx = (py * width + x) * 4;
            rgba_out[idx + 0] = r;
            rgba_out[idx + 1] = g;
            rgba_out[idx + 2] = b;
            rgba_out[idx + 3] = 255;
        }
    }

    /* ---- Waveform overlay (top wave_h rows) ---- */
    /* Draw center line */
    int wave_mid = wave_h / 2;
    for (int x = 0; x < width; x++) {
        int idx = (wave_mid * width + x) * 4;
        rgba_out[idx + 0] = 40;
        rgba_out[idx + 1] = 40;
        rgba_out[idx + 2] = 50;
        rgba_out[idx + 3] = 255;
    }

    /* Plot waveform from wave_buf */
    int wave_samples = sg->wave_cap;
    for (int x = 0; x < width; x++) {
        /* Map x to sample index in wave_buf */
        int si_base = (sg->wave_pos - width + x) % sg->wave_cap;
        if (si_base < 0) si_base += sg->wave_cap;

        float s = sg->wave_buf[si_base];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;

        int y_top = (int)((1.0f - s) * 0.5f * (float)(wave_h - 1));
        if (y_top < 0) y_top = 0;
        if (y_top >= wave_h) y_top = wave_h - 1;

        /* Draw vertical line from center to peak */
        int y_lo = (s >= 0.0f) ? wave_mid : y_top;
        int y_hi = (s >= 0.0f) ? y_top : wave_mid;
        if (y_lo > y_hi) { int tmp = y_lo; y_lo = y_hi; y_hi = tmp; }

        for (int y = y_lo; y <= y_hi; y++) {
            int idx = (y * width + x) * 4;
            rgba_out[idx + 0] = 120;
            rgba_out[idx + 1] = 220;
            rgba_out[idx + 2] = 255;
            rgba_out[idx + 3] = 255;
        }
    }

    /* ---- Separator line between waveform and spectrogram ---- */
    for (int x = 0; x < width; x++) {
        int idx = (wave_h * width + x) * 4;
        rgba_out[idx + 0] = 80;
        rgba_out[idx + 1] = 80;
        rgba_out[idx + 2] = 90;
        rgba_out[idx + 3] = 255;
    }

    return 0;
}

float wb_sonogram_get_peak(const wb_sonogram *sg) {
    if (!sg) return 0.0f;
    return sg->peak;
}

float wb_sonogram_get_rms(const wb_sonogram *sg) {
    if (!sg || sg->rms_frames == 0) return 0.0f;
    return sqrtf(sg->rms_accum / (float)sg->rms_frames);
}

float wb_sonogram_get_crest_factor(const wb_sonogram *sg) {
    if (!sg) return 0.0f;
    float rms = wb_sonogram_get_rms(sg);
    if (rms < 1e-10f) return 0.0f;
    return sg->peak / rms;
}

float wb_sonogram_get_spectral_centroid(const wb_sonogram *sg) {
    if (!sg) return 0.0f;
    return sg->spectral_centroid;
}