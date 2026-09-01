/* wb_noise_gate.c — Spectral noise gate for audio cleanup
 * R087: AI noise removal (Camtasia/Premiere/Resolve parity)
 *
 * Approach: FFT-based spectral gating. Learn noise profile from a silent
 * section, then attenuate frequency bins that fall below the noise floor.
 * Zero dependencies — uses our existing wb_fft (from wb_spectrum.c).
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wb_internal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WB_NG_MAX_BINS 2048

/* Struct definition (typedef in header) */
struct wb_noise_gate {
    float noise_floor[WB_NG_MAX_BINS];
    int   bins;
    int   learned;
    float sensitivity;
    float reduction;
    float attack_ms;
    float release_ms;
    float sample_rate;
    float envelope;
};

/* Create a noise gate instance */
wb_noise_gate *wb_noise_gate_create(float sample_rate) {
    wb_noise_gate *ng = (wb_noise_gate *)calloc(1, sizeof(wb_noise_gate));
    if (!ng) return NULL;
    ng->sample_rate = sample_rate;
    ng->sensitivity = 2.0f;
    ng->reduction = -40.0f;
    ng->attack_ms = 5.0f;
    ng->release_ms = 50.0f;
    ng->envelope = 1.0f;
    return ng;
}

void wb_noise_gate_destroy(wb_noise_gate *ng) {
    if (ng) free(ng);
}

void wb_noise_gate_set_params(wb_noise_gate *ng, float sensitivity, float reduction_db, float attack_ms, float release_ms) {
    if (!ng) return;
    ng->sensitivity = sensitivity;
    ng->reduction = reduction_db;
    ng->attack_ms = attack_ms;
    ng->release_ms = release_ms;
}

int wb_noise_gate_learn(wb_noise_gate *ng, const float *buf, int n_frames, int channels, int fft_size) {
    if (!ng || !buf || n_frames < fft_size || fft_size > WB_NG_MAX_BINS * 2) return -1;

    int bins = fft_size / 2;
    ng->bins = bins;
    memset(ng->noise_floor, 0, sizeof(ng->noise_floor));

    int hop = fft_size / 4;
    int frames = 0;

    float *window = (float *)calloc(fft_size, sizeof(float));
    float *mag = (float *)calloc(bins, sizeof(float));
    if (!window || !mag) { free(window); free(mag); return -1; }

    for (int i = 0; i < fft_size; i++)
        window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (fft_size - 1)));

    for (int pos = 0; pos + fft_size <= n_frames; pos += hop) {
        float re[WB_NG_MAX_BINS * 2];
        memset(re, 0, sizeof(re));

        for (int i = 0; i < fft_size; i++) {
            float s = 0;
            for (int c = 0; c < channels; c++)
                s += buf[(pos + i) * channels + c];
            s /= channels;
            re[i] = s * window[i];
        }

        for (int k = 0; k < bins; k++) {
            float sum_re = 0, sum_im = 0;
            for (int n = 0; n < fft_size; n++) {
                float angle = -2.0f * M_PI * k * n / fft_size;
                sum_re += re[n] * cosf(angle);
                sum_im += re[n] * sinf(angle);
            }
            mag[k] += sqrtf(sum_re * sum_re + sum_im * sum_im);
        }
        frames++;
    }

    if (frames > 0) {
        for (int k = 0; k < bins; k++)
            ng->noise_floor[k] = mag[k] / frames;
    }

    ng->learned = 1;
    free(window);
    free(mag);
    return 0;
}

int wb_noise_gate_process(wb_noise_gate *ng, float *buf, int n_frames, int channels) {
    if (!ng || !buf || !ng->learned) return -1;

    int fft_size = ng->bins * 2;
    if (n_frames < fft_size) return -1;

    float *window = (float *)calloc(fft_size, sizeof(float));
    float *re = (float *)calloc(fft_size, sizeof(float));
    float *im = (float *)calloc(fft_size, sizeof(float));
    if (!window || !re || !im) { free(window); free(re); free(im); return -1; }

    for (int i = 0; i < fft_size; i++)
        window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (fft_size - 1)));

    float reduction_linear = powf(10.0f, ng->reduction / 20.0f);
    float attack_coef = expf(-1.0f / (ng->attack_ms * ng->sample_rate / 1000.0f));
    float release_coef = expf(-1.0f / (ng->release_ms * ng->sample_rate / 1000.0f));

    int hop = fft_size / 4;
    float *output_buf = (float *)calloc(n_frames * channels, sizeof(float));
    float *overlap_sum = (float *)calloc(n_frames, sizeof(float));
    if (!output_buf || !overlap_sum) {
        free(window); free(re); free(im); free(output_buf); free(overlap_sum);
        return -1;
    }

    for (int pos = 0; pos + fft_size <= n_frames; pos += hop) {
        for (int i = 0; i < fft_size; i++) {
            float s = 0;
            for (int c = 0; c < channels; c++)
                s += buf[(pos + i) * channels + c];
            s /= channels;
            re[i] = s * window[i];
            im[i] = 0;
        }

        /* Forward DFT */
        for (int k = 0; k < ng->bins; k++) {
            float sum_re = 0, sum_im = 0;
            for (int n = 0; n < fft_size; n++) {
                float angle = -2.0f * M_PI * k * n / fft_size;
                sum_re += re[n] * cosf(angle);
                sum_im += re[n] * sinf(angle);
            }
            re[k] = sum_re;
            im[k] = sum_im;
        }

        /* Spectral gating per bin */
        float frame_gate = 1.0f;
        for (int k = 0; k < ng->bins; k++) {
            float mag = sqrtf(re[k] * re[k] + im[k] * im[k]);
            float threshold = ng->noise_floor[k] * ng->sensitivity;

            if (mag < threshold) {
                float gain = reduction_linear;
                re[k] *= gain;
                im[k] *= gain;
                if (gain < frame_gate) frame_gate = gain;
            }
        }

        /* Smooth envelope */
        if (frame_gate < ng->envelope)
            ng->envelope = attack_coef * ng->envelope + (1.0f - attack_coef) * frame_gate;
        else
            ng->envelope = release_coef * ng->envelope + (1.0f - release_coef) * frame_gate;

        /* Inverse DFT */
        for (int n = 0; n < fft_size; n++) {
            float sum = 0;
            for (int k = 0; k < ng->bins; k++) {
                float angle = 2.0f * M_PI * k * n / fft_size;
                sum += re[k] * cosf(angle) - im[k] * sinf(angle);
            }
            sum /= fft_size;

            float windowed = sum * window[n];
            for (int c = 0; c < channels; c++)
                output_buf[(pos + n) * channels + c] += windowed;
            overlap_sum[pos + n] += window[n] * window[n];
        }
    }

    /* Normalize overlap-add and write back */
    for (int i = 0; i < n_frames; i++) {
        if (overlap_sum[i] > 0.001f) {
            float norm = 1.0f / overlap_sum[i];
            for (int c = 0; c < channels; c++)
                buf[i * channels + c] = output_buf[i * channels + c] * norm;
        }
    }

    free(window); free(re); free(im);
    free(output_buf); free(overlap_sum);
    return 0;
}
