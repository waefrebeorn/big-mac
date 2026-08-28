/* wb_drum_simd.c — G5: SIMD-accelerated drum render.
 *
 * The drum path has sin() calls in kick (freq sweep), snare (220Hz), and toms (note freq).
 * With DRUM_VOICES=8, that's up to 8 sin() calls per sample when all voices active.
 *
 * Strategy: batch 4 voices at a time through vec_sin_4 for the sine-based drums.
 * Noise-based drums (snare/hat/clap/crash) don't use sin() — they stay scalar.
 *
 * Key insight: drums are transient, so most voices are inactive most of the time.
 * The SIMD path is only beneficial when multiple sine drums are simultaneous.
 * We use a simple heuristic: if ≥2 sine voices active in a batch of 4, use SIMD.
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "g2_fm_simd.h"

typedef float wb_sample;

#define TWO_PI 6.2831853071795864769
#define DRUM_VOICES 8

typedef struct { int note; int active; int kind; double env; double t; double f0; double phstep; } drum_voice;

typedef struct {
    uint32_t sr;
    drum_voice v[DRUM_VOICES];
} drum_inst;

/* ---- External ---- */
extern double wb_midi_note_to_freq(int note);

/* ---- Scalar helpers (mirror of wb_drum_render switch cases) ---- */
static inline float drum_sin_kick(drum_voice *v, double inv_SR) {
    double f = 80.0 - (v->t * inv_SR / 0.05) * 20.0;
    if (f < 40) f = 40;
    return (float)(sin(v->t * TWO_PI * f * inv_SR) * v->env);
}

static inline float drum_sin_tom(drum_voice *v) {
    return (float)(sin(v->t * v->phstep) * v->env * 0.5);
}

static inline float drum_sin_snare(drum_voice *v, double snare_ps) {
    float noise = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
    float tone = (float)(sin(v->t * snare_ps) * v->env * 0.3);
    return noise * (float)v->env * 0.5f + tone;
}

/* ---- SIMD drum render ---- */
void wb_drum_render_simd(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    drum_inst *d = inst;
    double inv_SR = 1.0 / (double)d->sr;

    /* Precompute per-kind envelope decay constants */
    double env_decay[9];
    for (int k = 0; k < 9; k++) {
        double dec = 0.10;
        switch (k) {
            case 0: dec = 0.08; break;
            case 8: dec = 0.30; break;
            case 2: dec = 0.04; break;
            case 4: dec = 0.12; break;
            default: dec = 0.10; break;
        }
        env_decay[k] = exp(-1.0 / (dec * d->sr));
    }
    double snare_phase_step = TWO_PI * 220.0 * inv_SR;

    for (uint32_t i = 0; i < n; i++) {
        float sL = L[i], sR = R[i];

        /* Process drums in 2 batches of 4 voices */
        for (int b = 0; b < DRUM_VOICES; b += 4) {
            /* Identify sine-based voices in this batch */
            int sine_idx[4];   /* voice indices that use sin */
            int sine_count = 0;
            int noise_only = 0; /* voices that are noise-only */

            for (int j = 0; j < 4; j++) {
                drum_voice *v = &d->v[b + j];
                if (!v->active) continue;
                if (v->kind == 0 || v->kind == 1 || v->kind >= 5) {
                    /* kick, snare, toms — use sin */
                    sine_idx[sine_count++] = j;
                } else {
                    /* hat/clap/crash — noise only */
                    noise_only++;
                }
            }

            if (sine_count >= 2) {
                /* SIMD path: batch sin for multiple voices */
                float phases[4] = {0,0,0,0};
                float envs[4] = {0,0,0,0};
                float scales[4] = {0,0,0,0};  /* per-voice amplitude scale */

                for (int s = 0; s < sine_count; s++) {
                    int j = sine_idx[s];
                    drum_voice *v = &d->v[b + j];
                    v->t += 1.0;

                    switch (v->kind) {
                        case 0: { /* kick */
                            double f = 80.0 - (v->t * inv_SR / 0.05) * 20.0;
                            if (f < 40) f = 40;
                            phases[s] = (float)(v->t * TWO_PI * f * inv_SR);
                            envs[s] = (float)v->env;
                            scales[s] = 1.0f;
                            break;
                        }
                        case 1: { /* snare */
                            phases[s] = (float)(v->t * snare_phase_step);
                            envs[s] = (float)v->env;
                            scales[s] = 0.3f;
                            break;
                        }
                        default: { /* tom */
                            phases[s] = (float)(v->t * v->phstep);
                            envs[s] = (float)v->env;
                            scales[s] = 0.5f;
                            break;
                        }
                    }
                }

                /* Pad unused lanes with 0 */
                while (sine_count < 4) {
                    phases[sine_count] = 0.0f;
                    envs[sine_count] = 0.0f;
                    scales[sine_count] = 0.0f;
                    sine_count++;
                }

                __m128 phase_vec = _mm_loadu_ps(phases);
                __m128 sin_vec = vec_sin_4(phase_vec);
                __m128 env_vec = _mm_loadu_ps(envs);
                __m128 scale_vec = _mm_loadu_ps(scales);
                __m128 result = _mm_mul_ps(sin_vec, _mm_mul_ps(env_vec, scale_vec));

                float outs[4];
                _mm_storeu_ps(outs, result);

                /* Apply outputs */
                for (int s = 0; s < sine_count && s < 4; s++) {
                    int j = sine_idx[s];
                    drum_voice *v = &d->v[b + j];

                    float out = outs[s];
                    /* Add noise component for snare */
                    if (v->kind == 1) {
                        float noise = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
                        out += noise * (float)v->env * 0.5f;
                    }

                    v->env *= env_decay[v->kind < 9 ? v->kind : 0];
                    if (v->env < 0.001) v->active = 0;

                    sL += out;
                    sR += out;
                }
            } else {
                /* Scalar path for <2 sine voices */
                for (int j = 0; j < 4; j++) {
                    drum_voice *v = &d->v[b + j];
                    if (!v->active) continue;
                    v->t += 1.0;
                    float out = 0;

                    switch (v->kind) {
                        case 0: {
                            double f = 80.0 - (v->t * inv_SR / 0.05) * 20.0;
                            if (f < 40) f = 40;
                            out = (float)(sin(v->t * TWO_PI * f * inv_SR) * v->env);
                            break;
                        }
                        case 1: {
                            float noise = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
                            out = noise * (float)v->env * 0.5f;
                            out += (float)(sin(v->t * snare_phase_step) * v->env * 0.3);
                            break;
                        }
                        case 2: case 3: case 4: case 8: {
                            float noise = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
                            float scale = (v->kind == 2 || v->kind == 4) ? 0.4f :
                                          (v->kind == 3) ? 0.5f : 0.3f;
                            out = noise * (float)v->env * scale;
                            break;
                        }
                        default: {
                            out = (float)(sin(v->t * v->phstep) * v->env * 0.5);
                        }
                    }

                    v->env *= env_decay[v->kind < 9 ? v->kind : 0];
                    if (v->env < 0.001) { v->active = 0; continue; }
                    sL += out;
                    sR += out;
                }
            }
        }

        L[i] = sL;
        R[i] = sR;
    }
}
