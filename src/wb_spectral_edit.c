/* wb_spectral_edit.c — spectral audio repair tools (iZotope RX style).
 * Pure C11, zero third-party. Uses wb_fft for STFT.
 * Spectral gating denoise, transient-based declick, harmonic notch dehum,
 * and time-domain gain. */
#include "wbus/wbus_spectral_edit.h"
#include "wbus/wbus_fft.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SE_FRAME   2048
#define SE_HOP     512
#define SE_FFT_N   2048

/* ---- Hann window ---- */
static void make_hann(double *w, int n) {
    for (int i = 0; i < n; i++)
        w[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (n - 1)));
}

/* ---- dB <-> linear ---- */
static inline double db2lin(double db) { return pow(10.0, db / 20.0); }

/* ---- Overlap-add STFT state ---- */
typedef struct {
    double window[SE_FRAME];
    double mag[SE_FRAME];
    double phase[SE_FRAME];
} se_stft;

static void stft_analyze(se_stft *s, const wb_sample *x, int n,
                         const wb_fft_plan *plan, double *re, double *im) {
    /* zero-pad to FFT size */
    for (int i = 0; i < SE_FFT_N; i++) re[i] = 0.0;
    int copy = n < SE_FRAME ? n : SE_FRAME;
    for (int i = 0; i < copy; i++) re[i] = (double)x[i] * s->window[i];
    wb_fft_run(plan, re, im, 0);
    for (int i = 0; i < SE_FFT_N; i++) {
        s->mag[i]   = sqrt(re[i]*re[i] + im[i]*im[i]);
        s->phase[i] = atan2(im[i], re[i]);
    }
}

static void stft_synthesize(const se_stft *s, wb_sample *out, int n,
                            const wb_fft_plan *plan, double *re, double *im) {
    for (int i = 0; i < SE_FFT_N; i++) {
        re[i] = s->mag[i] * cos(s->phase[i]);
        im[i] = s->mag[i] * sin(s->phase[i]);
    }
    double tmp[SE_FFT_N];
    wb_fft_real_inverse(plan, re, im, tmp);
    int copy = n < SE_FRAME ? n : SE_FRAME;
    for (int i = 0; i < copy; i++)
        out[i] = (wb_sample)(tmp[i] * s->window[i]);
}

/* ================================================================
 * wb_spectral_gain — simple dB gain in time domain
 * ================================================================ */
int wb_spectral_gain(const wb_sample *in, wb_sample *out, uint32_t frames,
                     uint32_t chn, float gain_db) {
    if (!in || !out || frames == 0 || chn == 0) return -1;
    double g = db2lin((double)gain_db);
    uint32_t total = frames * chn;
    for (uint32_t i = 0; i < total; i++)
        out[i] = (wb_sample)((double)in[i] * g);
    return 0;
}

/* ================================================================
 * wb_spectral_denoise — spectral gating
 * Estimate noise floor from first 100ms, attenuate bins below
 * threshold proportional to strength.
 * ================================================================ */
int wb_spectral_denoise(const wb_sample *in, wb_sample *out, uint32_t frames,
                        uint32_t chn, float strength) {
    if (!in || !out || frames == 0 || chn == 0) return -1;
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
    if (strength == 0.0f) {
        uint32_t total = frames * chn;
        memcpy(out, in, total * sizeof(wb_sample));
        return 0;
    }

    /* Use zero-padded overlap-add to avoid boundary artifacts.
     * Pad by SE_FRAME on each side for full overlap coverage. */
    uint32_t pad = SE_FRAME; /* generous padding */
    uint32_t padded_frames = frames + 2 * pad;
    /* Accumulator covers padded region plus one extra frame for the tail */
    uint32_t acc_size = padded_frames + SE_FRAME * 2;

    /* Noise profile estimation window: first 100ms of ORIGINAL signal */
    uint32_t noise_samples = (uint32_t)(WB_SAMPLE_RATE / 10); /* 100ms */
    if (noise_samples > frames) noise_samples = frames;

    wb_fft_plan *plan = wb_fft_create(SE_FFT_N);
    if (!plan) return -1;

    double *re = (double*)malloc(SE_FFT_N * sizeof(double));
    double *im = (double*)malloc(SE_FFT_N * sizeof(double));
    double *window = (double*)malloc(SE_FRAME * sizeof(double));
    double *noise_floor = (double*)calloc(SE_FFT_N / 2 + 1, sizeof(double));
    /* Accumulator for padded length */
    double *acc = (double*)calloc(acc_size * chn, sizeof(double));
    double *win_sum = (double*)calloc(acc_size * chn, sizeof(double));
    if (!re || !im || !window || !noise_floor || !acc || !win_sum) {
        free(re); free(im); free(window); free(noise_floor); free(acc); free(win_sum);
        wb_fft_destroy(plan);
        return -1;
    }

    make_hann(window, SE_FRAME);

    /* Estimate noise floor from first 100ms (channel 0) */
    {
        uint32_t pos = 0;
        uint32_t frame_count = 0;
        while (pos + SE_FRAME <= noise_samples) {
            double frame[SE_FFT_N];
            for (int i = 0; i < SE_FFT_N; i++) frame[i] = 0.0;
            for (int i = 0; i < SE_FRAME; i++)
                frame[i] = (double)in[(pos + i) * chn] * window[i];
            for (int i = 0; i < SE_FFT_N; i++) { re[i] = frame[i]; im[i] = 0.0; }
            wb_fft_run(plan, re, im, 0);
            for (int k = 0; k <= SE_FFT_N / 2; k++) {
                double mag = sqrt(re[k]*re[k] + im[k]*im[k]);
                noise_floor[k] += mag;
            }
            frame_count++;
            pos += SE_HOP;
        }
        if (frame_count > 0) {
            for (int k = 0; k <= SE_FFT_N / 2; k++)
                noise_floor[k] /= (double)frame_count;
        }
        /* Cap extreme values at 3x global mean for robustness */
        double global_sum = 0.0;
        for (int k = 0; k <= SE_FFT_N / 2; k++) global_sum += noise_floor[k];
        double global_mean = global_sum / (double)(SE_FFT_N / 2 + 1);
        for (int k = 0; k <= SE_FFT_N / 2; k++) {
            if (noise_floor[k] > 3.0 * global_mean)
                noise_floor[k] = 3.0 * global_mean;
        }
    }

    /* Process each channel with zero-padded overlap-add */
    for (uint32_t c = 0; c < chn; c++) {
        memset(acc, 0, acc_size * chn * sizeof(double));
        memset(win_sum, 0, acc_size * chn * sizeof(double));

        /* Process padded signal */
        uint32_t pos = 0;
        while (pos < padded_frames + SE_FRAME) {
            /* Extract frame from padded position */
            double frame[SE_FFT_N];
            for (int i = 0; i < SE_FFT_N; i++) frame[i] = 0.0;
            for (int i = 0; i < SE_FRAME; i++) {
                int src_idx = (int)(pos + i) - (int)pad; /* position in original signal */
                if (src_idx >= 0 && src_idx < (int)frames)
                    frame[i] = (double)in[(int)src_idx * chn + c] * window[i];
                /* else: zero padding */
            }

            for (int i = 0; i < SE_FFT_N; i++) { re[i] = frame[i]; im[i] = 0.0; }
            wb_fft_run(plan, re, im, 0);

            /* Spectral gating */
            double strength_d = (double)strength;
            double thresh_low = 1.0 + strength_d * 2.5;
            double thresh_high = thresh_low + 2.0;
            double gain_floor = 0.05 + (1.0 - strength_d) * 0.95;
            for (int k = 0; k <= SE_FFT_N / 2; k++) {
                double mag_sq = re[k]*re[k] + im[k]*im[k];
                double nf_sq = noise_floor[k] * noise_floor[k];
                double gain;
                if (nf_sq < 1e-40) {
                    gain = 1.0;
                } else {
                    double ratio = mag_sq / nf_sq;
                    double lt = thresh_low * thresh_low;
                    double ht = thresh_high * thresh_high;
                    if (ratio >= ht) {
                        gain = 1.0;
                    } else if (ratio >= lt) {
                        double t = (ratio - lt) / (ht - lt);
                        gain = gain_floor + (1.0 - gain_floor) * t;
                    } else {
                        gain = gain_floor;
                    }
                }
                re[k] *= gain;
                im[k] *= gain;
                if (k > 0 && k < SE_FFT_N / 2) {
                    re[SE_FFT_N - k] *= gain;
                    im[SE_FFT_N - k] *= gain;
                }
            }

            double tmp[SE_FFT_N];
            wb_fft_real_inverse(plan, re, im, tmp);

            /* Overlap-add into padded accumulator */
            for (int i = 0; i < SE_FRAME; i++) {
                uint32_t idx = (pos + i) * chn + c;
                acc[idx] += tmp[i] * window[i];
                win_sum[idx] += window[i] * window[i];
            }

            pos += SE_HOP;
        }

        /* Extract center portion (original signal region) and normalize */
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t pad_idx = (i + pad) * chn + c;
            double ws = win_sum[pad_idx];
            if (ws > 1e-10)
                out[i * chn + c] = (wb_sample)(acc[pad_idx] / ws);
            else
                out[i * chn + c] = in[i * chn + c];
        }
    }

    free(re); free(im); free(window); free(noise_floor); free(acc); free(win_sum);
    wb_fft_destroy(plan);
    return 0;
}

/* ================================================================
 * wb_spectral_declick — detect transient spikes via derivative
 * threshold, interpolate affected samples.
 * ================================================================ */
int wb_spectral_declick(const wb_sample *in, wb_sample *out, uint32_t frames,
                        uint32_t chn, float threshold) {
    if (!in || !out || frames == 0 || chn == 0) return -1;
    if (threshold <= 0.0f) threshold = 0.01f;

    uint32_t total = frames * chn;
    memcpy(out, in, total * sizeof(wb_sample));

    for (uint32_t c = 0; c < chn; c++) {
        /* Compute derivative (diff) and find spikes */
        float *diff = (float*)malloc(frames * sizeof(float));
        if (!diff) return -1;

        diff[0] = 0.0f;
        for (uint32_t i = 1; i < frames; i++)
            diff[i] = fabsf(in[i * chn + c] - in[(i - 1) * chn + c]);

        /* Find click regions: consecutive samples where diff > threshold */
        uint32_t i = 1;
        while (i < frames - 1) {
            if (diff[i] > threshold) {
                /* Found a click start — find extent (up to 32 samples) */
                uint32_t start = i > 2 ? i - 2 : 0;
                uint32_t end = start + 32;
                if (end >= frames) end = frames - 1;
                /* Extend while derivative is elevated */
                while (end < frames - 1 && end < start + 64) {
                    if (diff[end] > threshold * 0.5f) end++;
                    else break;
                }

                /* Interpolate between start-1 and end+1 */
                float left = (start > 0) ? out[(start - 1) * chn + c] : 0.0f;
                float right = (end < frames - 1) ? out[(end + 1) * chn + c] : left;
                uint32_t span = end - start + 1;
                for (uint32_t j = start; j <= end && j < frames; j++) {
                    float t = (span > 1) ? (float)(j - start + 1) / (float)(span + 1) : 0.5f;
                    out[j * chn + c] = left + (right - left) * t;
                }
                i = end + 1;
            } else {
                i++;
            }
        }
        free(diff);
    }
    return 0;
}

/* ================================================================
 * wb_spectral_dehum — notch filter at hum_freq and harmonics up to 1kHz
 * Uses spectral domain: zero out bins at hum_freq harmonics.
 * ================================================================ */
int wb_spectral_dehum(const wb_sample *in, wb_sample *out, uint32_t frames,
                      uint32_t chn, float hum_freq) {
    if (!in || !out || frames == 0 || chn == 0) return -1;
    if (hum_freq <= 0.0f) hum_freq = 60.0f;

    uint32_t total = frames * chn;
    memcpy(out, in, total * sizeof(wb_sample));

    wb_fft_plan *plan = wb_fft_create(SE_FFT_N);
    if (!plan) return -1;

    double *re = (double*)malloc(SE_FFT_N * sizeof(double));
    double *im = (double*)malloc(SE_FFT_N * sizeof(double));
    double *window = (double*)malloc(SE_FRAME * sizeof(double));
    double *acc = (double*)calloc(total, sizeof(double));
    double *win_sum = (double*)calloc(total, sizeof(double));
    if (!re || !im || !window || !acc || !win_sum) {
        free(re); free(im); free(window); free(acc); free(win_sum);
        wb_fft_destroy(plan);
        return -1;
    }

    make_hann(window, SE_FRAME);

    /* Compute bin width */
    double bin_width = (double)WB_SAMPLE_RATE / (double)SE_FFT_N;

    /* Build notch bin set: hum_freq, 2*hum_freq, ... up to 1000 Hz */
    int max_harmonics = (int)(1000.0 / hum_freq);
    if (max_harmonics < 1) max_harmonics = 1;
    if (max_harmonics > 20) max_harmonics = 20;

    int notch_bins[21];
    int notch_count = 0;
    for (int h = 1; h <= max_harmonics; h++) {
        double freq = hum_freq * (double)h;
        int bin = (int)round(freq / bin_width);
        if (bin > 0 && bin < SE_FFT_N / 2) {
            notch_bins[notch_count++] = bin;
        }
    }

    /* Notch width: ±2 bins around each harmonic */
    int notch_half_width = 2;

    for (uint32_t c = 0; c < chn; c++) {
        memset(acc, 0, total * sizeof(double));
        memset(win_sum, 0, total * sizeof(double));

        uint32_t pos = 0;
        while (pos + SE_FRAME <= frames) {
            for (int i = 0; i < SE_FFT_N; i++) re[i] = 0.0;
            for (int i = 0; i < SE_FRAME; i++)
                re[i] = (double)in[(pos + i) * chn + c] * window[i];
            for (int i = 0; i < SE_FFT_N; i++) im[i] = 0.0;

            wb_fft_run(plan, re, im, 0);

            /* Apply notch filters at hum harmonics */
            for (int n = 0; n < notch_count; n++) {
                int center = notch_bins[n];
                for (int k = center - notch_half_width; k <= center + notch_half_width; k++) {
                    if (k >= 0 && k <= SE_FFT_N / 2) {
                        /* Smooth notch: cosine taper */
                        double dist = fabs((double)k - (double)center) / (double)(notch_half_width + 1);
                        double gain = dist * dist; /* 0 at center, 1 at edge */
                        if (k == center) gain = 0.0;
                        else if (dist >= 1.0) gain = 1.0;

                        re[k] *= gain;
                        im[k] *= gain;
                        if (k > 0 && k < SE_FFT_N / 2) {
                            re[SE_FFT_N - k] *= gain;
                            im[SE_FFT_N - k] *= gain;
                        }
                    }
                }
            }

            double tmp[SE_FFT_N];
            wb_fft_real_inverse(plan, re, im, tmp);

            for (int i = 0; i < SE_FRAME; i++) {
                acc[(pos + i) * chn + c] += tmp[i] * window[i];
                win_sum[(pos + i) * chn + c] += window[i] * window[i];
            }
            pos += SE_HOP;
        }

        /* Normalize and write */
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t idx = i * chn + c;
            double ws = win_sum[idx];
            if (ws > 1e-10)
                out[idx] = (wb_sample)(acc[idx] / ws);
            else
                out[idx] = in[idx];
        }
    }

    free(re); free(im); free(window); free(acc); free(win_sum);
    wb_fft_destroy(plan);
    return 0;
}