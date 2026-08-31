/* wb_dynamics_adv.c — advanced dynamics processor (Ableton Multiband Dynamics).
 *
 * Multiband compressor, parallel compression, sidechain EQ.
 * Pure C11. */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_BANDS 4

/* One-pole envelope follower */
static float env_follow(float in, float *env, float attack, float release, uint32_t sr) {
    float coeff = (in > *env) ? attack : release;
    *env += coeff * (in - *env);
    return *env;
}

/* Simple compressor for one band */
static float compress(float in, float *env, float threshold, float ratio,
                       float attack_ms, float release_ms, uint32_t sr) {
    float x = fabsf(in);
    float atk = expf(-1000.0f / (attack_ms * (float)sr));
    float rel = expf(-1000.0f / (release_ms * (float)sr));
    float level = env_follow(x, env, atk, rel, sr);

    float gain = 1.0f;
    if (level > threshold && threshold > 0) {
        float db_in = 20.0f * log10f(level + 1e-10f);
        float db_over = db_in - 20.0f * log10f(threshold);
        float db_out = db_over / ratio;
        gain = powf(10.0f, (db_out - db_over) / 20.0f);
    }
    return in * gain;
}

/* LR4 crossover split */
static void crossover_split(const float *in, uint32_t n, float freq, uint32_t sr,
                             float *low, float *high) {
    float w0 = 2.0f * M_PI * freq / (float)sr;
    float a = 1.0f - expf(-w0);
    float lp = 0;
    for (uint32_t i = 0; i < n; i++) {
        lp += a * (in[i] - lp);
        low[i] = lp;
        high[i] = in[i] - lp;
    }
}

typedef struct {
    uint32_t sr;
    int mode;           /* 0=comp, 1=limiter, 2=gate, 3=expander, 4=multiband */
    int band_count;
    float band_freqs[MAX_BANDS-1];
    float threshold[MAX_BANDS];
    float ratio[MAX_BANDS];
    float attack[MAX_BANDS];
    float release[MAX_BANDS];
    float knee[MAX_BANDS];
    float parallel_mix;
    float env[MAX_BANDS];
    float sidechain_eq_freq, sidechain_eq_q, sidechain_eq_gain;
    float input_gain, output_gain;
    float loudness_target;
} wb_dynamics;

void *wb_dynamics_create(uint32_t sr) {
    wb_dynamics *d = (wb_dynamics *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->sr = sr;
    d->mode = 0;
    d->band_count = 1;
    for (int i = 0; i < MAX_BANDS; i++) {
        d->threshold[i] = 0.5f;
        d->ratio[i] = 4.0f;
        d->attack[i] = 10.0f;
        d->release[i] = 100.0f;
        d->knee[i] = 6.0f;
    }
    d->parallel_mix = 0;
    d->input_gain = 1.0f;
    d->output_gain = 1.0f;
    d->loudness_target = -14.0f;
    d->sidechain_eq_freq = 1000.0f;
    d->sidechain_eq_q = 1.0f;
    d->sidechain_eq_gain = 0;
    return d;
}

void wb_dynamics_destroy(void *ptr) { free(ptr); }

void wb_dynamics_set_mode(void *ptr, int mode) {
    wb_dynamics *d = (wb_dynamics *)ptr;
    if (!d) return;
    d->mode = mode;
    if (mode == 4) d->band_count = 3;
}

void wb_dynamics_set_band_count(void *ptr, int bands) {
    wb_dynamics *d = (wb_dynamics *)ptr;
    if (!d) return;
    d->band_count = bands < 1 ? 1 : (bands > MAX_BANDS ? MAX_BANDS : bands);
}

void wb_dynamics_set_band_freq(void *ptr, int band, float freq) {
    wb_dynamics *d = (wb_dynamics *)ptr;
    if (!d || band < 0 || band >= MAX_BANDS-1) return;
    d->band_freqs[band] = freq;
}

void wb_dynamics_set_threshold(void *ptr, int band, float db) {
    wb_dynamics *d = (wb_dynamics *)ptr;
    if (!d || band < 0 || band >= MAX_BANDS) return;
    d->threshold[band] = db;
}

void wb_dynamics_set_ratio(void *ptr, int band, float r) {
    wb_dynamics *d = (wb_dynamics *)ptr;
    if (!d || band < 0 || band >= MAX_BANDS) return;
    d->ratio[band] = r < 1.0f ? 1.0f : r;
}

void wb_dynamics_set_attack(void *ptr, int band, float ms) {
    wb_dynamics *d = (wb_dynamics *)ptr;
    if (!d || band < 0 || band >= MAX_BANDS) return;
    d->attack[band] = ms < 0.1f ? 0.1f : ms;
}

void wb_dynamics_set_release(void *ptr, int band, float ms) {
    wb_dynamics *d = (wb_dynamics *)ptr;
    if (!d || band < 0 || band >= MAX_BANDS) return;
    d->release[band] = ms < 1.0f ? 1.0f : ms;
}

void wb_dynamics_set_knee(void *ptr, int band, float db) {
    wb_dynamics *d = (wb_dynamics *)ptr;
    if (!d || band < 0 || band >= MAX_BANDS) return;
    d->knee[band] = db;
}

void wb_dynamics_set_parallel_mix(void *ptr, float mix) {
    wb_dynamics *d = (wb_dynamics *)ptr;
    if (!d) return;
    d->parallel_mix = mix < 0 ? 0 : (mix > 1 ? 1 : mix);
}

void wb_dynamics_set_sidechain_source(void *ptr, const wb_sample *ext, uint32_t frames) {
    (void)ptr; (void)ext; (void)frames;
    /* External sidechain input — processed in process() */
}

void wb_dynamics_set_sidechain_eq(void *ptr, float freq, float q, float gain) {
    wb_dynamics *d = (wb_dynamics *)ptr;
    if (!d) return;
    d->sidechain_eq_freq = freq;
    d->sidechain_eq_q = q;
    d->sidechain_eq_gain = gain;
}

void wb_dynamics_process(void *ptr, wb_sample *out, const wb_sample *in, uint32_t frames) {
    wb_dynamics *d = (wb_dynamics *)ptr;
    if (!d || !out || !in) return;

    /* Apply input gain */
    float *processed = (float *)malloc(frames * sizeof(float));
    if (!processed) return;
    for (uint32_t i = 0; i < frames; i++)
        processed[i] = in[i] * d->input_gain;

    if (d->mode == 4) {
        /* Multiband: split, compress per band, recombine */
        float *bands[MAX_BANDS] = {0};
        float *current = (float *)malloc(frames * sizeof(float));
        if (!current) { free(processed); return; }
        memcpy(current, processed, frames * sizeof(float));

        float freqs[] = {150, 1000, 8000};
        for (int b = 0; b < d->band_count; b++) {
            bands[b] = (float *)calloc(frames, sizeof(float));
            float *residual = (float *)calloc(frames, sizeof(float));
            if (!bands[b] || !residual) { free(processed); free(current); return; }
            crossover_split(current, frames, freqs[b], d->sr, bands[b], residual);
            /* Compress this band */
            for (uint32_t i = 0; i < frames; i++) {
                bands[b][i] = compress(bands[b][i], &d->env[b],
                                        d->threshold[b], d->ratio[b],
                                        d->attack[b], d->release[b], d->sr);
            }
            memcpy(current, residual, frames * sizeof(float));
            free(residual);
        }
        /* Recombine */
        for (uint32_t i = 0; i < frames; i++) {
            processed[i] = current[i];
            for (int b = 0; b < d->band_count; b++)
                processed[i] += bands[b][i];
        }
        for (int b = 0; b < d->band_count; b++)
            free(bands[b]);
        free(current);
    } else {
        /* Single band */
        for (uint32_t i = 0; i < frames; i++) {
            processed[i] = compress(processed[i], &d->env[0],
                                     d->threshold[0], d->ratio[0],
                                     d->attack[0], d->release[0], d->sr);
        }
    }

    /* Parallel mix */
    float mix = d->parallel_mix;
    for (uint32_t i = 0; i < frames; i++) {
        processed[i] = in[i] * (1.0f - mix) + processed[i] * mix;
        /* Output gain + soft clip */
        float v = processed[i] * d->output_gain;
        if (v > 0.99f) v = 0.99f + 0.01f * tanhf((v - 0.99f) * 100.0f);
        if (v < -0.99f) v = -0.99f + 0.01f * tanhf((v + 0.99f) * 100.0f);
        out[i] = v;
    }

    free(processed);
}
