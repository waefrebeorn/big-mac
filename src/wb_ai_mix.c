/* wb_ai_mix.c — AI-assisted mixing tools (Logic Mastering Assistant style).
 *
 * Analyzes audio and applies automatic EQ, level, pan placement, and de-essing.
 * Pure C11, zero third-party. Reuses wb_fft.c (spectrum) and wb_lufs.c
 * (K-weighted BS.1770 loudness).
 *
 * Design notes:
 *   - All functions operate on mono (single-channel) buffers for the analysis
 *     API, matching the wb_sample=float convention.  The host runs them per-
 *     channel or on a mono sum as needed.
 *   - wb_lufs_* expects a single-channel buffer; we pass it directly.
 *   - The engine's wb_biquad_set() type set (lowpass/highpass/bandpass/notch)
 *     does not include a peaking or shelf type, so we compute RBJ coefficients
 *     directly for the EQ and de-ess high-shelf.
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "wbus.h"
#include "wbus_fft.h"
#include "wbus_lufs.h"
#include "wbus_dsp.h"
#include "wbus_ai_mix.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int ai_next_pow2(int n) {
    if (n < 2) return 2;
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* RBJ peaking EQ — the engine's wb_biquad_set() doesn't support a peaking
 * type (gain_db is "reserved").  We compute coefficients here using the
 * audio-cookbook formula and run the biquad manually. */
typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} ai_peaking;

static void ai_peaking_set(ai_peaking *p, float sr, float freq, float q,
                           float gain_db) {
    float w0 = 2.0f * (float)M_PI * freq / sr;
    float cw = cosf(w0), sw = sinf(w0);
    float A  = powf(10.0f, gain_db / 40.0f);
    float alpha = sw / (2.0f * q);
    /* RBJ peaking EQ (audio cookbook). a0 normalises by 1 + alpha/A. */
    float a0 = 1.0f + alpha / A;
    p->b0 = (1.0f + alpha * A) / a0;
    p->b1 = (-2.0f * cw) / a0;
    p->b2 = (1.0f - alpha * A) / a0;
    p->a1 = (-2.0f * cw) / a0;
    p->a2 = (1.0f - alpha / A) / a0;
    p->x1 = p->x2 = p->y1 = p->y2 = 0.0f;
}

static float ai_peaking_process(ai_peaking *p, float x) {
    float y = p->b0 * x + p->b1 * p->x1 + p->b2 * p->x2
              - p->a1 * p->y1 - p->a2 * p->y2;
    p->x2 = p->x1; p->x1 = x;
    p->y2 = p->y1; p->y1 = y;
    return y;
}

/* RBJ high-shelf (for de-ess HF attenuation). */
typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} ai_shelf;

static void ai_highshelf_set(ai_shelf *p, float sr, float freq, float q,
                             float gain_db) {
    float w0   = 2.0f * (float)M_PI * freq / sr;
    float cw   = cosf(w0), sw = sinf(w0);
    float A    = powf(10.0f, gain_db / 40.0f);
    float alpha = sw / (2.0f * q);
    float a0   = (A + 1.0f) - (A - 1.0f) * cw + 2.0f * sqrtf(A) * alpha;
    p->b0 =        A * ((A + 1.0f) - (A - 1.0f) * cw + 2.0f * sqrtf(A) * alpha) / a0;
    p->b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw) / a0;
    p->b2 =        A * ((A + 1.0f) + (A - 1.0f) * cw - 2.0f * sqrtf(A) * alpha) / a0;
    p->a1 =        -2.0f * ((A - 1.0f) + (A + 1.0f) * cw) / a0;
    p->a2 = ((A + 1.0f) - (A - 1.0f) * cw - 2.0f * sqrtf(A) * alpha) / a0;
    p->x1 = p->x2 = p->y1 = p->y2 = 0.0f;
}

static float ai_shelf_process(ai_shelf *p, float x) {
    float y = p->b0 * x + p->b1 * p->x1 + p->b2 * p->x2
              - p->a1 * p->y1 - p->a2 * p->y2;
    p->x2 = p->x1; p->x1 = x;
    p->y2 = p->y1; p->y1 = y;
    return y;
}

/* One-pole high-pass for HF energy extraction (de-ess). */
typedef struct {
    float alpha;
    float prev;
} ai_hpf;

static void ai_hpf_init(ai_hpf *h, float sr, float cutoff) {
    float rc   = 1.0f / (2.0f * (float)M_PI * cutoff);
    float dt   = 1.0f / sr;
    h->alpha   = rc / (rc + dt);
    h->prev    = 0.0f;
}

static float ai_hpf_process(ai_hpf *h, float x) {
    float y = h->alpha * (h->prev + x - h->prev);
    h->prev = x;
    return y;
}

/* ------------------------------------------------------------------ */
/* 8 octave-band centre frequencies (log-spaced, 31.5 Hz – 4 kHz)      */
/* ------------------------------------------------------------------ */

#define AI_NUM_BANDS 8

static const float ai_band_centers[AI_NUM_BANDS] = {
    31.5f, 63.0f, 125.0f, 250.0f,
    500.0f, 1000.0f, 2000.0f, 4000.0f
};

/* "Warm" target curve: gentle LF lift and HF roll-off. */
static const float ai_target_warm[AI_NUM_BANDS] = {
     0.0f,  0.0f,  0.0f,  1.0f,
     0.0f,  0.0f, -0.5f, -2.0f
};

/* Flat target curve: 0 dB everywhere (available for future "flat" mode).</n * Currently unused (warm is the default), kept for API completeness. */
static const float ai_target_flat[AI_NUM_BANDS] __attribute__((unused)) = {
     0.0f,  0.0f,  0.0f,  0.0f,
     0.0f,  0.0f,  0.0f,  0.0f
};

/* Band energy: sum squared magnitudes of FFT bins whose centre frequency
 * falls within [lo, hi) Hz.  Returns the energy (sum of |X[k]|^2). */
static double ai_band_energy(const double *re, const double *im,
                             int nfft, double sr, double lo, double hi) {
    int bin_lo = (int)(lo / sr * nfft);
    int bin_hi = (int)(hi / sr * nfft);
    if (bin_lo < 0) bin_lo = 0;
    if (bin_hi > nfft / 2) bin_hi = nfft / 2;
    if (bin_hi <= bin_lo) return 0.0;
    double e = 0.0;
    for (int k = bin_lo; k < bin_hi; k++) {
        double m = re[k] * re[k] + im[k] * im[k];
        e += m;
    }
    return e;
}

/* ------------------------------------------------------------------ */
/* Analyze                                                            */
/* ------------------------------------------------------------------ */

int wb_ai_mix_analyze(const wb_sample *audio, uint32_t frames, uint32_t sr,
                      float *rms_out, float *peak_out,
                      float *loudness_lufs_out, float *crest_factor_out,
                      float *spectral_centroid_out) {
    if (!audio || sr == 0) return -1;

    /* RMS + peak */
    double sq_sum = 0.0, peak = 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        double x = (double)audio[i];
        sq_sum += x * x;
        if (fabs(x) > peak) peak = fabs(x);
    }
    double rms = 0.0;
    if (frames > 0) rms = sqrt(sq_sum / (double)frames);

    if (rms_out)           *rms_out           = (float)rms;
    if (peak_out)          *peak_out          = (float)peak;

    /* Crest factor (dB) */
    if (crest_factor_out) {
        if (rms > 1e-10 && peak > 1e-10)
            *crest_factor_out = (float)(20.0 * log10(peak / rms));
        else
            *crest_factor_out = 0.0f;
    }

    /* LUFS via wb_lufs (K-weighted, BS.1770) */
    if (loudness_lufs_out) {
        wb_lufs lufs;
        wb_lufs_create(&lufs, (double)sr);
        uint32_t chunk = 8192;
        for (uint32_t pos = 0; pos < frames; pos += chunk) {
            uint32_t n = frames - pos;
            if (n > chunk) n = chunk;
            wb_lufs_process(&lufs, &audio[pos], (int)n);
        }
        float lufs_val = (float)wb_lufs_integrated_lufs(&lufs);
        /* wb_lufs integrated returns 0.0 when no 400 ms gate block has
         * closed (signal shorter than the gate window).  Fall back to a
         * simplified BS.1770 estimate from raw mean-square. */
        if (lufs_val == 0.0f && rms > 0.0) {
            /* LUFS = -0.691 + 10*log10(mean_square) */
            lufs_val = (float)(-0.691 + 10.0 * log10(rms * rms));
        }
        *loudness_lufs_out = lufs_val;
    }

    /* Spectral centroid via FFT */
    if (spectral_centroid_out) {
        int nfft = ai_next_pow2((frames > 0) ? (int)frames : 2);
        if (nfft < 4) nfft = 4;
        wb_fft_plan *plan = wb_fft_create(nfft);
        if (plan) {
            double *x  = (double *)calloc((size_t)nfft, sizeof(double));
            double *re = (double *)calloc((size_t)nfft, sizeof(double));
            double *im = (double *)calloc((size_t)nfft, sizeof(double));
            if (x && re && im) {
                for (uint32_t i = 0; i < frames && i < (uint32_t)nfft; i++)
                    x[i] = (double)audio[i];
                /* Hann window */
                for (int i = 0; i < nfft; i++) {
                    double w = 0.5 * (1.0 - cos(2.0 * M_PI * i / (nfft - 1)));
                    x[i] *= w;
                }
                wb_fft_real(plan, x, re, im);
                double wsum = 0.0, mag_sum = 0.0;
                for (int k = 0; k < nfft / 2; k++) {
                    double mag = sqrt(re[k] * re[k] + im[k] * im[k]);
                    double freq = (double)k * (double)sr / (double)nfft;
                    wsum   += mag * freq;
                    mag_sum += mag;
                }
                if (mag_sum > 1e-20)
                    *spectral_centroid_out = (float)(wsum / mag_sum);
                else
                    *spectral_centroid_out = 0.0f;
            } else {
                *spectral_centroid_out = 0.0f;
            }
            free(x); free(re); free(im);
            wb_fft_destroy(plan);
        } else {
            *spectral_centroid_out = 0.0f;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Auto-EQ                                                            */
/* ------------------------------------------------------------------ */

int wb_ai_mix_auto_eq(const wb_sample *in, wb_sample *out,
                      uint32_t frames, uint32_t sr,
                      float *suggested_gains_db, int num_bands) {
    if (!in || !out || sr == 0 || frames == 0) return -1;

    int nb = num_bands;
    if (nb < 1) nb = 1;
    if (nb > AI_NUM_BANDS) nb = AI_NUM_BANDS;

    /* --- 1. FFT analysis --- */
    int nfft = ai_next_pow2((int)frames);
    if (nfft < 64) nfft = 64;
    /* Cap at 32768 to keep allocation reasonable */
    if (nfft > 32768) nfft = 32768;

    wb_fft_plan *plan = wb_fft_create(nfft);
    double *x  = NULL, *re = NULL, *im = NULL;
    if (!plan) {
        /* Fallback: copy input, zero gains */
        memcpy(out, in, frames * sizeof(wb_sample));
        if (suggested_gains_db) memset(suggested_gains_db, 0, nb * sizeof(float));
        return 0;
    }
    x  = (double *)calloc((size_t)nfft, sizeof(double));
    re = (double *)calloc((size_t)nfft, sizeof(double));
    im = (double *)calloc((size_t)nfft, sizeof(double));
    if (!x || !re || !im) {
        free(x); free(re); free(im); wb_fft_destroy(plan);
        memcpy(out, in, frames * sizeof(wb_sample));
        if (suggested_gains_db) memset(suggested_gains_db, 0, nb * sizeof(float));
        return 0;
    }

    for (uint32_t i = 0; i < frames && i < (uint32_t)nfft; i++)
        x[i] = (double)in[i];
    /* Hann window */
    for (int i = 0; i < nfft; i++) {
        double w = 0.5 * (1.0 - cos(2.0 * M_PI * i / (nfft - 1)));
        x[i] *= w;
    }
    wb_fft_real(plan, x, re, im);

    /* --- 2. Per-band energy vs target --- */
    /* Use a "warm" target curve */
    float gains[AI_NUM_BANDS];
    for (int b = 0; b < AI_NUM_BANDS; b++) {
        double lo = (double)ai_band_centers[b] / 1.5f;  /* ~half-octave below */
        double hi = (double)ai_band_centers[b] * 1.5f;  /* ~half-octave above */
        double band_e = ai_band_energy(re, im, nfft, (double)sr, lo, hi);
        double band_db = (band_e > 1e-20) ? 10.0 * log10(band_e / (double)nfft) : -90.0;
        double target_db = (double)ai_target_warm[b];
        double diff = target_db - band_db;
        /* Clamp to ±6 dB */
        if (diff > 6.0) diff = 6.0;
        if (diff < -6.0) diff = -6.0;
        gains[b] = (float)diff;
    }

    /* Fill caller's suggestion array */
    if (suggested_gains_db) {
        for (int b = 0; b < nb; b++)
            suggested_gains_db[b] = gains[b];
    }

    /* --- 3. Apply EQ: cascade of peaking filters --- */
    ai_peaking f[AI_NUM_BANDS];
    for (int b = 0; b < AI_NUM_BANDS; b++) {
        float q = 0.9f;  /* ~1.4 octave bandwidth */
        ai_peaking_set(&f[b], (float)sr, ai_band_centers[b], q, gains[b]);
    }

    for (uint32_t i = 0; i < frames; i++) {
        float y = in[i];
        for (int b = 0; b < AI_NUM_BANDS; b++)
            y = ai_peaking_process(&f[b], y);
        out[i] = y;
    }

    free(x); free(re); free(im);
    wb_fft_destroy(plan);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Auto-level (loudness normalization)                                */
/* ------------------------------------------------------------------ */

int wb_ai_mix_auto_level(const wb_sample *in, wb_sample *out,
                         uint32_t frames, uint32_t sr,
                         float target_lufs) {
    if (!in || !out || sr == 0) return -1;
    if (frames == 0) return 0;

    /* Measure integrated LUFS */
    wb_lufs lufs;
    wb_lufs_create(&lufs, (double)sr);
    uint32_t chunk = 8192;
    for (uint32_t pos = 0; pos < frames; pos += chunk) {
        uint32_t n = frames - pos;
        if (n > chunk) n = chunk;
        wb_lufs_process(&lufs, &in[pos], (int)n);
    }
    float measured_lufs = (float)wb_lufs_integrated_lufs(&lufs);

    /* wb_lufs returns 0.0 when no 400 ms gate block closed (short signal). */
    if (measured_lufs == 0.0f) {
        double sq_sum = 0.0;
        for (uint32_t i = 0; i < frames; i++) {
            double x = (double)in[i];
            sq_sum += x * x;
        }
        double rms = sqrt(sq_sum / (double)frames);
        if (rms > 0.0)
            measured_lufs = (float)(-0.691 + 10.0 * log10(rms * rms));
    }

    /* Compute gain */
    float gain_db;
    if (measured_lufs <= -90.0f || measured_lufs == 0.0f) {
        /* Silence or no data — no gain change */
        gain_db = 0.0f;
    } else {
        gain_db = target_lufs - measured_lufs;
        /* Limit headroom: cap at +20 dB to prevent blow-up */
        if (gain_db > 20.0f) gain_db = 20.0f;
        if (gain_db < -50.0f) gain_db = -50.0f;
    }

    float gain_lin = powf(10.0f, gain_db / 20.0f);

    for (uint32_t i = 0; i < frames; i++)
        out[i] = in[i] * gain_lin;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Suggest pan                                                        */
/* ------------------------------------------------------------------ */

int wb_ai_mix_suggest_pan(uint32_t track_count,
                          const float *spectral_centroids,
                          float *suggested_pans) {
    if (!spectral_centroids || !suggested_pans) return -1;
    if (track_count == 0) return 0;

    /* Pairwise sort by spectral centroid (ascending).  We use a simple
     * insertion sort since track_count is small (≤128 typically).        */
    int order[256];
    uint32_t n = track_count;
    if (n > 256) n = 256;
    for (uint32_t i = 0; i < n; i++) order[i] = (int)i;

    for (uint32_t i = 1; i < n; i++) {
        int key = order[i];
        int j = (int)i - 1;
        while (j >= 0 && spectral_centroids[order[j]] > spectral_centroids[key]) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    /* Distribute: alternate left/right so spectrally-similar tracks land
     * on opposite sides.  Rank 0 → -1.0, rank 1 → +1.0, rank 2 → -0.7,
     * rank 3 → +0.7, etc.  Clamped to [-1, 1].                        */
    for (uint32_t r = 0; r < n; r++) {
        float spread = 1.0f - (float)r / (float)(n > 1 ? n : 1);  /* 1→0 */
        float sign = (r % 2 == 0) ? -1.0f : 1.0f;
        suggested_pans[order[r]] = sign * spread;
    }

    /* For a single track, centre it */
    if (n == 1) suggested_pans[order[0]] = 0.0f;

    return 0;
}

/* ------------------------------------------------------------------ */
/* De-ess                                                             */
/* ------------------------------------------------------------------ */

int wb_ai_mix_de_ess(const wb_sample *in, wb_sample *out,
                     uint32_t frames, uint32_t sr,
                     float threshold_db) {
    if (!in || !out || sr == 0) return -1;
    if (frames == 0) return 0;

    /* Extract HF content (high-pass at 5 kHz) and compute HF/total energy
     * ratio over the whole buffer.  If the ratio (in dB) exceeds
     * threshold_db, attenuate the HF band with a high-shelf.           */
    float hf_cutoff = 5000.0f;

    double hf_sq = 0.0, total_sq = 0.0;

    ai_hpf hf_filter;
    ai_hpf_init(&hf_filter, (float)sr, hf_cutoff);

    for (uint32_t i = 0; i < frames; i++) {
        float x = in[i];
        total_sq += (double)x * (double)x;
        float h = ai_hpf_process(&hf_filter, x);
        hf_sq += (double)h * (double)h;
    }

    double total_energy = total_sq;
    if (total_energy < 1e-20) {
        /* Silence — just copy */
        memcpy(out, in, frames * sizeof(wb_sample));
        return 0;
    }

    double hf_rms     = sqrt(hf_sq / (double)frames);
    double total_rms  = sqrt(total_energy / (double)frames);

    if (hf_rms < 1e-10) {
        memcpy(out, in, frames * sizeof(wb_sample));
        return 0;
    }

    float ratio_db = (float)(20.0 * log10(hf_rms / total_rms));

    if (ratio_db <= threshold_db) {
        /* Below threshold — no de-essing needed */
        memcpy(out, in, frames * sizeof(wb_sample));
        return 0;
    }

    /* Attenuate HF band: the more we exceed the threshold, the more
     * reduction we apply (max -12 dB).                              */
    float over_db = ratio_db - threshold_db;
    float reduction_db = -over_db;  /* proportional */
    if (reduction_db < -12.0f) reduction_db = -12.0f;

    /* Apply a high-shelf centred at 5 kHz with the computed reduction */
    ai_shelf hs;
    ai_highshelf_set(&hs, (float)sr, hf_cutoff, 0.7f, reduction_db);

    for (uint32_t i = 0; i < frames; i++) {
        out[i] = ai_shelf_process(&hs, in[i]);
    }

    return 0;
}
