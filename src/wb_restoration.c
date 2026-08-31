/* src/wb_restoration.c — audio restoration tools (iZotope RX style).
 * Pure C11, zero third-party. Spectral subtraction denoise, median-filter
 * declip, cubic-interpolation declip, harmonic notch dehum, and bandpass
 * + spectral-gate voice isolation. All process mono float buffers. */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus.h"
#include "wbus/wbus_fft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WB_RESTORATION_SR 44100

/* ---- internal helpers ------------------------------------------------ */

static float wb_fabsf(float x) { return x < 0.0f ? -x : x; }

/* ---- Denoise: spectral subtraction with noise profile ---------------- */
/* Estimate noise floor from first 100ms, subtract oversubtracted spectrum. */

int wb_restoration_denoise(const float *in, float *out, int n, float strength) {
    if (!in || !out || n <= 0) return -1;
    if (strength <= 0.0f) {
        memcpy(out, in, (size_t)n * sizeof(float));
        return 0;
    }
    if (strength > 1.0f) strength = 1.0f;

    /* Determine FFT size: next power of two >= n */
    int fft_size = 1;
    while (fft_size < n) fft_size <<= 1;
    if (fft_size < 512) fft_size = 512;

    double *re = (double *)calloc(fft_size, sizeof(double));
    double *im = (double *)calloc(fft_size, sizeof(double));
    double *noise_profile = (double *)calloc((size_t)(fft_size / 2 + 1), sizeof(double));
    if (!re || !im || !noise_profile) {
        free(re); free(im); free(noise_profile);
        return -1;
    }

    /* Noise profile from first 100ms (or whole signal if shorter) */
    int noise_frames = (int)(0.1 * WB_RESTORATION_SR);
    if (noise_frames > n) noise_frames = n;
    if (noise_frames < 64) noise_frames = n; /* too short: use whole signal */

    /* Use same FFT size for noise estimation (zero-pad the noise segment) */
    double *n_re = (double *)calloc(fft_size, sizeof(double));
    double *n_im = (double *)calloc(fft_size, sizeof(double));
    if (!n_re || !n_im) {
        free(re); free(im); free(noise_profile); free(n_re); free(n_im);
        return -1;
    }

    for (int i = 0; i < noise_frames; i++)
        n_re[i] = (double)in[i];

    wb_fft_plan *fp = wb_fft_create(fft_size);
    wb_fft_run(fp, n_re, n_im, 0);

    /* Compute noise magnitude spectrum (per-bin average) */
    double noise_scale = 1.0; /* single-frame estimate */
    for (int i = 0; i <= fft_size / 2; i++) {
        noise_profile[i] = sqrt(n_re[i] * n_re[i] + n_im[i] * n_im[i]) * noise_scale;
    }

    /* Forward FFT of full signal */
    for (int i = 0; i < n; i++)
        re[i] = (double)in[i];

    wb_fft_run(fp, re, im, 0);

    /* Spectral subtraction with flooring.
     * alpha: oversubtraction factor (1.0 = exact, >1 = more aggressive)
     * beta: spectral floor (fraction of original magnitude to keep) */
    float alpha = 1.0f + 2.0f * strength; /* 1..3 */
    float beta = 0.05f + 0.15f * (1.0f - strength); /* 0.05..0.2 */

    for (int i = 0; i <= fft_size / 2; i++) {
        double mag = sqrt(re[i] * re[i] + im[i] * im[i]);
        double phase = atan2(im[i], re[i]);
        double np_i = noise_profile[i] * alpha;
        double new_mag = mag - np_i;
        /* Apply spectral floor: never go below beta * original magnitude */
        double floor_val = beta * mag;
        if (new_mag < floor_val) new_mag = floor_val;
        if (new_mag < 0.0) new_mag = 0.0;
        re[i] = new_mag * cos(phase);
        im[i] = new_mag * sin(phase);
        /* Mirror for conjugate-symmetric bins */
        if (i > 0 && i < fft_size / 2) {
            re[fft_size - i] = re[i];
            im[fft_size - i] = -im[i];
        }
    }

    /* Inverse FFT (wb_fft_run with invert=1 already normalizes by 1/n) */
    wb_fft_run(fp, re, im, 1);

    /* Copy output (already normalized by IFFT) */
    for (int i = 0; i < n; i++) {
        double v = re[i];
        /* Clamp to prevent overflow */
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        out[i] = (float)v;
    }

    wb_fft_destroy(fp);
    free(re); free(im); free(noise_profile); free(n_re); free(n_im);
    return 0;
}

/* ---- Declick: median-filter transient detection + interpolation ------ */

static float median5(float a, float b, float c, float d, float e) {
    /* Sorting network for 5 elements */
    float t;
    if (a > b) { t = a; a = b; b = t; }
    if (c > d) { t = c; c = d; d = t; }
    if (a > c) { t = a; a = c; c = t; t = b; b = d; d = t; }
    if (b > e) { t = b; b = e; e = t; }
    if (b > c) { t = b; b = c; c = t; }
    if (c > e) { t = c; c = e; e = t; }
    return c;
}

int wb_restoration_declick(const float *in, float *out, int n, float threshold) {
    if (!in || !out || n <= 0) return -1;
    if (threshold <= 0.0f) {
        memcpy(out, in, (size_t)n * sizeof(float));
        return 0;
    }

    /* Copy input first */
    memcpy(out, in, (size_t)n * sizeof(float));

    /* Detect clicks: large deviation from local median.
     * For each sample, compare against median of surrounding 5 samples. */
    int *click_positions = (int *)calloc((size_t)n, sizeof(int));
    if (!click_positions) return -1;

    int click_count = 0;

    for (int i = 2; i < n - 2; i++) {
        float med = median5(in[i-2], in[i-1], in[i], in[i+1], in[i+2]);
        float dev = wb_fabsf(in[i] - med);
        if (dev > threshold) {
            click_positions[click_count++] = i;
        }
    }

    /* Interpolate clicked samples using cubic interpolation from neighbors */
    for (int c = 0; c < click_count; c++) {
        int pos = click_positions[c];
        /* Find extent of click burst */
        int end = pos;
        while (end + 1 < n && end - pos < 16) {
            int found = 0;
            for (int k = c + 1; k < click_count; k++) {
                if (click_positions[k] == end + 1) { found = 1; break; }
                if (click_positions[k] > end + 1) break;
            }
            if (!found) break;
            end++;
        }
        /* Skip past the burst in the outer loop */
        c += (end - pos);

        /* Interpolate from clean samples on each side */
        int left = pos - 1;
        int right = end + 1;
        if (left < 0 || right >= n) {
            /* Edge: just copy nearest clean sample */
            float val = (left >= 0) ? out[left] : out[right];
            for (int j = pos; j <= end; j++) out[j] = val;
            continue;
        }

        /* Linear interpolation (robust for short bursts) */
        float y0 = out[left];
        float y1 = out[right];
        int span = right - left;
        for (int j = pos; j <= end; j++) {
            float t = (float)(j - left) / (float)span;
            out[j] = y0 + t * (y1 - y0);
        }
    }

    free(click_positions);
    return 0;
}

/* ---- Declip: detect clipped samples + cubic reconstruction ----------- */

int wb_restoration_declip(const float *in, float *out, int n, float threshold) {
    if (!in || !out || n <= 0) return -1;
    if (threshold <= 0.0f || threshold > 1.0f) threshold = 0.95f;

    memcpy(out, in, (size_t)n * sizeof(float));

    /* Find clipped runs: samples at or near ±threshold */
    int i = 0;
    while (i < n) {
        if (wb_fabsf(in[i]) >= threshold) {
            /* Found clipped sample; find extent */
            int start = i;
            while (i < n && wb_fabsf(in[i]) >= threshold) i++;
            int end = i - 1;

            /* Reconstruct via cubic interpolation from 4 surrounding samples */
            int i0 = start - 2;
            int i1 = start - 1;
            int i2 = end + 1;
            int i3 = end + 2;

            /* Clamp indices */
            if (i0 < 0) i0 = 0;
            if (i1 < 0) i1 = 0;
            if (i2 >= n) i2 = n - 1;
            if (i3 >= n) i3 = n - 1;

            float y0 = out[i0], y1 = out[i1], y2 = out[i2], y3 = out[i3];

            for (int j = start; j <= end; j++) {
                /* Lagrange cubic interpolation */
                float t = 0.0f;
                if (i2 != i1)
                    t = (float)(j - i1) / (float)(i2 - i1);
                /* Catmull-Rom spline */
                float t2 = t * t;
                float t3 = t2 * t;
                float v = 0.5f * (
                    (2.0f * y1) +
                    (-y0 + y2) * t +
                    (2.0f*y0 - 5.0f*y1 + 4.0f*y2 - y3) * t2 +
                    (-y0 + 3.0f*y1 - 3.0f*y2 + y3) * t3
                );
                /* Clamp output */
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                out[j] = v;
            }
        } else {
            i++;
        }
    }

    return 0;
}

/* ---- Dehum: notch filter at hum_freq + harmonics -------------------- */

/* Simple 2nd-order IIR notch filter */
typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2, y1, y2;
} biquad_notch;

static void notch_init(biquad_notch *f, float freq, float sr, float bw) {
    float w0 = 2.0f * M_PI * freq / sr;
    float alpha = sinf(w0) * sinhf(logf(2.0f) / 2.0f * bw * w0 / sinf(w0));
    float a0 = 1.0f + alpha;
    f->b0 = 1.0f / a0;
    f->b1 = (-2.0f * cosf(w0)) / a0;
    f->b2 = 1.0f / a0;
    f->a1 = f->b1;
    f->a2 = (1.0f - alpha) / a0;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}

static float notch_process(biquad_notch *f, float x) {
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
              - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    return y;
}

int wb_restoration_dehum(const float *in, float *out, int n, float hum_freq) {
    if (!in || !out || n <= 0) return -1;
    if (hum_freq <= 0.0f) hum_freq = 60.0f;

    /* Apply notch at hum_freq and harmonics up to ~4kHz */
    memcpy(out, in, (size_t)n * sizeof(float));

    float sr = (float)WB_RESTORATION_SR;
    float max_freq = sr / 2.0f - 100.0f; /* stay below Nyquist */
    float bw = 2.0f; /* bandwidth in Hz (narrow notch) */

    for (float freq = hum_freq; freq <= max_freq && freq < 4000.0f; freq += hum_freq) {
        biquad_notch notch;
        notch_init(&notch, freq, sr, bw);
        /* Warm up the filter to avoid transient */
        for (int i = 0; i < n; i++) {
            out[i] = notch_process(&notch, out[i]);
        }
    }

    return 0;
}

/* ---- Voice isolation: bandpass 300Hz-4kHz + spectral gating ---------- */

int wb_restoration_voice_isolate(const float *in, float *out, int n, float strength) {
    if (!in || !out || n <= 0) return -1;
    if (strength <= 0.0f) {
        memcpy(out, in, (size_t)n * sizeof(float));
        return 0;
    }
    if (strength > 1.0f) strength = 1.0f;

    float sr = (float)WB_RESTORATION_SR;

    /* Step 1: Bandpass filter 300Hz - 4kHz using cascaded biquad sections */
    /* High-pass at 300Hz */
    float hp_w0 = 2.0f * M_PI * 300.0f / sr;
    float hp_cos = cosf(hp_w0);
    float hp_sin = sinf(hp_w0);
    float hp_Q = 0.707f;
    float hp_alpha = hp_sin / (2.0f * hp_Q);
    float hp_a0 = 1.0f + hp_alpha;
    float hp_b0 = (1.0f + hp_cos) / (2.0f * hp_a0);
    float hp_b1 = -(1.0f + hp_cos) / hp_a0;
    float hp_b2 = hp_b0;
    float hp_a1 = (-2.0f * hp_cos) / hp_a0;
    float hp_a2 = (1.0f - hp_alpha) / hp_a0;

    /* Low-pass at 4kHz */
    float lp_w0 = 2.0f * M_PI * 4000.0f / sr;
    float lp_cos = cosf(lp_w0);
    float lp_sin = sinf(lp_w0);
    float lp_Q = 0.707f;
    float lp_alpha = lp_sin / (2.0f * lp_Q);
    float lp_a0 = 1.0f + lp_alpha;
    float lp_b0 = (1.0f - lp_cos) / (2.0f * lp_a0);
    float lp_b1 = (1.0f - lp_cos) / lp_a0;
    float lp_b2 = lp_b0;
    float lp_a1 = (-2.0f * lp_cos) / lp_a0;
    float lp_a2 = (1.0f - lp_alpha) / lp_a0;

    /* Apply bandpass */
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < n; i++) {
        float x = in[i];
        float y = hp_b0 * x + hp_b1 * x1 + hp_b2 * x2 - hp_a1 * y1 - hp_a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        out[i] = y;
    }

    /* Second pass: low-pass */
    x1 = x2 = y1 = y2 = 0.0f;
    for (int i = 0; i < n; i++) {
        float x = out[i];
        float y = lp_b0 * x + lp_b1 * x1 + lp_b2 * x2 - lp_a1 * y1 - lp_a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        out[i] = y;
    }

    /* Step 2: Spectral gating to suppress non-voice content */
    /* Use overlap-add STFT with spectral gating */
    int frame_size = 1024;
    int hop = frame_size / 4; /* 75% overlap */
    if (n < frame_size) {
        /* Signal too short for STFT: just return bandpass result */
        /* Mix with original based on strength */
        for (int i = 0; i < n; i++)
            out[i] = in[i] + strength * (out[i] - in[i]);
        return 0;
    }

    int fft_size = frame_size;
    double *spec_re = (double *)calloc(fft_size, sizeof(double));
    double *spec_im = (double *)calloc(fft_size, sizeof(double));
    double *windowed = (double *)calloc(fft_size, sizeof(double));
    float *result_buf = (float *)calloc((size_t)n, sizeof(float));
    float *window_sum = (float *)calloc((size_t)n, sizeof(float));

    if (!spec_re || !spec_im || !windowed || !result_buf || !window_sum) {
        free(spec_re); free(spec_im); free(windowed);
        free(result_buf); free(window_sum);
        return -1;
    }

    /* Hann window */
    double *window = (double *)calloc(fft_size, sizeof(double));
    for (int i = 0; i < fft_size; i++)
        window[i] = 0.5 * (1.0 - cos(2.0 * M_PI * (double)i / (double)(fft_size - 1)));

    wb_fft_plan *fp = wb_fft_create(fft_size);

    /* Estimate noise floor from first frame (assumed silence/noise) */
    double noise_mag[513]; /* fft_size/2 + 1 */
    for (int i = 0; i < frame_size && i < n; i++)
        windowed[i] = (double)out[i] * window[i];
    for (int i = frame_size; i < fft_size; i++)
        windowed[i] = 0.0;

    wb_fft_real(fp, windowed, spec_re, spec_im);
    for (int b = 0; b <= fft_size / 2; b++)
        noise_mag[b] = sqrt(spec_re[b] * spec_re[b] + spec_im[b] * spec_im[b]);

    /* Process all frames */
    int num_frames = 0;
    for (int offset = 0; offset < n; offset += hop) {
        /* Apply window and FFT */
        for (int i = 0; i < fft_size; i++) {
            int idx = offset + i;
            if (idx < n)
                windowed[i] = (double)out[idx] * window[i];
            else
                windowed[i] = 0.0;
        }

        wb_fft_real(fp, windowed, spec_re, spec_im);

        /* Spectral gate: gentle attenuation of bins far above noise floor.
         * This is a soft gate — only suppress bins that are clearly noise. */
        float gate_ratio = 0.3f + 0.4f * (1.0f - strength); /* keep 30-70% minimum */
        for (int b = 0; b <= fft_size / 2; b++) {
            double mag = sqrt(spec_re[b] * spec_re[b] + spec_im[b] * spec_im[b]);
            double threshold = noise_mag[b] * 3.0; /* 3x noise floor */
            double ratio = 1.0;
            if (threshold > 1e-10 && mag < threshold) {
                /* Soft gate: scale proportionally between threshold and noise floor */
                if (mag < noise_mag[b]) {
                    ratio = gate_ratio; /* floor for very quiet bins */
                } else {
                    double t = (mag - noise_mag[b]) / (threshold - noise_mag[b]);
                    ratio = gate_ratio + (1.0 - gate_ratio) * t;
                }
            }
            spec_re[b] *= ratio;
            spec_im[b] *= ratio;
            /* Maintain conjugate symmetry for real output */
            if (b > 0 && b < fft_size / 2) {
                spec_re[fft_size - b] = spec_re[b];
                spec_im[fft_size - b] = -spec_im[b];
            }
        }

        /* Inverse FFT */
        wb_fft_real_inverse(fp, spec_re, spec_im, windowed);

        /* Overlap-add */
        for (int i = 0; i < fft_size; i++) {
            int idx = offset + i;
            if (idx < n) {
                result_buf[idx] += (float)(windowed[i] * window[i]);
                window_sum[idx] += (float)(window[i] * window[i]);
            }
        }
        num_frames++;
    }

    /* Normalize by window sum */
    for (int i = 0; i < n; i++) {
        if (window_sum[i] > 1e-6f)
            out[i] = result_buf[i] / window_sum[i];
        else
            out[i] = result_buf[i];
    }

    /* Mix with original based on strength */
    for (int i = 0; i < n; i++)
        out[i] = in[i] + strength * (out[i] - in[i]);

    wb_fft_destroy(fp);
    free(spec_re); free(spec_im); free(windowed);
    free(result_buf); free(window_sum); free(window);
    return 0;
}