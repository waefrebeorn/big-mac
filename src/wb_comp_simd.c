/* wb_comp_simd.c — SIMD compressor/limiter (4 independent compressors in parallel).
 *
 * Processes 4 independent compressor instances simultaneously using SSE2.
 * Each compressor still processes samples serially (envelope follower is
 * inherently sequential), but the 4 instances run in SIMD parallel across
 * SSE2 lanes. This gives 4× throughput for multi-track compression.
 *
 * G6 [R075]: compressor/limiter sidechain SIMD.
 * Pure C11, SSE2 intrinsics. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <emmintrin.h>
#include "wbus.h"

/* ---- Scalar helpers ---- */
static inline float db_to_lin(float db) { return powf(10.0f, db / 20.0f); }

/* ---- SIMD compressor: 4 independent instances in parallel ---- */

typedef struct {
    float threshold_db;
    float ratio;
    float knee;
    float attack_ms;
    float release_ms;
    float makeup_db;
    __m128 env;         /* smoothed envelope (4 lanes) */
    __m128 att_coeff;
    __m128 rel_coeff;
    __m128 makeup;
} wb_comp4_inst;

void *wb_comp4_create(uint32_t sr) {
    wb_comp4_inst *c = (wb_comp4_inst *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->threshold_db = -12.0f;
    c->ratio = 4.0f;
    c->knee = 6.0f;
    c->attack_ms = 5.0f;
    c->release_ms = 120.0f;
    c->makeup_db = 0.0f;
    c->env = _mm_setzero_ps();

    float att = expf(-1.0f / (c->attack_ms * 0.001f * (float)sr));
    float rel = expf(-1.0f / (c->release_ms * 0.001f * (float)sr));
    c->att_coeff = _mm_set1_ps(att);
    c->rel_coeff = _mm_set1_ps(rel);
    c->makeup = _mm_set1_ps(db_to_lin(c->makeup_db));
    return c;
}

void wb_comp4_destroy(void *inst) { free(inst); }

void wb_comp4_set(void *inst, int param, float v) {
    wb_comp4_inst *c = (wb_comp4_inst *)inst;
    if (!c) return;
    switch (param) {
    case 0: c->threshold_db = v; break;
    case 1: c->ratio = v; break;
    case 2: c->makeup_db = v; break;
    default: break;
    }
}

/* Process 4 independent stereo compressor instances in parallel.
 * L/R are __m128 arrays where each element is a 4-lane vector:
 *   L[i] = {inst0_L, inst1_L, inst2_L, inst3_L} */
void wb_comp4_process(void *inst, __m128 *L, __m128 *R, uint32_t n) {
    wb_comp4_inst *c = (wb_comp4_inst *)inst;
    if (!c) return;

    __m128 att = c->att_coeff;
    __m128 rel = c->rel_coeff;
    __m128 makeup = c->makeup;
    __m128 one = _mm_set1_ps(1.0f);
    __m128 env = c->env;

    for (uint32_t i = 0; i < n; i++) {
        __m128 l = L[i];
        __m128 r = R[i];

        /* Peak detection: max(|L|, |R|) per instance */
        __m128 abs_l = _mm_andnot_ps(_mm_set1_ps(-0.0f), l);
        __m128 abs_r = _mm_andnot_ps(_mm_set1_ps(-0.0f), r);
        __m128 peak = _mm_max_ps(abs_l, abs_r);

        /* Extract to scalar for log10/pow (no SIMD log in SSE2) */
        float peak_arr[4], target_arr[4];
        _mm_storeu_ps(peak_arr, peak);

        for (int j = 0; j < 4; j++) {
            float db = 20.0f * log10f(peak_arr[j] + 1e-9f);
            float gain_db = 0.0f;
            float slope = 1.0f / c->ratio;
            if (db > (c->threshold_db - c->knee * 0.5f) &&
                db < (c->threshold_db + c->knee * 0.5f)) {
                float x = db - c->threshold_db;
                gain_db = (db - c->threshold_db) * (slope - 1.0f)
                        + (x * x / (2.0f * c->knee)) * (1.0f - slope);
            } else if (db >= (c->threshold_db + c->knee * 0.5f)) {
                gain_db = (db - c->threshold_db) * (slope - 1.0f);
            }
            target_arr[j] = powf(10.0f, gain_db / 20.0f);
        }

        /* Load back to SIMD for envelope follower */
        __m128 target_gain = _mm_loadu_ps(target_arr);

        /* Envelope follower: attack if target < env, release otherwise */
        __m128 attack_branch = _mm_add_ps(_mm_mul_ps(att, env),
            _mm_mul_ps(_mm_sub_ps(one, att), target_gain));
        __m128 release_branch = _mm_add_ps(_mm_mul_ps(rel, env),
            _mm_mul_ps(_mm_sub_ps(one, rel), target_gain));
        __m128 is_attack = _mm_cmplt_ps(target_gain, env);
        env = _mm_or_ps(_mm_and_ps(is_attack, attack_branch),
                        _mm_andnot_ps(is_attack, release_branch));

        /* Apply gain * makeup */
        __m128 g = _mm_mul_ps(env, makeup);
        L[i] = _mm_mul_ps(l, g);
        R[i] = _mm_mul_ps(r, g);
    }

    c->env = env;
}

/* ---- Scalar reference compressor (for testing) ---- */

typedef struct {
    float threshold_db;
    float ratio;
    float knee;
    float attack_ms;
    float release_ms;
    float makeup_db;
    float env;
    uint32_t sr;
} wb_comp_ref;

void *wb_comp_ref_create(uint32_t sr) {
    wb_comp_ref *c = (wb_comp_ref *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->sr = sr;
    c->threshold_db = -12.0f;
    c->ratio = 4.0f;
    c->knee = 6.0f;
    c->attack_ms = 5.0f;
    c->release_ms = 120.0f;
    c->makeup_db = 0.0f;
    return c;
}

void wb_comp_ref_destroy(void *inst) { free(inst); }

void wb_comp_ref_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_comp_ref *c = (wb_comp_ref *)inst;
    if (!c) return;
    float thresh = c->threshold_db;
    float slope = 1.0f / (c->ratio > 0 ? c->ratio : 4.0f);
    float knee = c->knee;
    float att = expf(-1.0f / (c->attack_ms * 0.001f * c->sr));
    float rel = expf(-1.0f / (c->release_ms * 0.001f * c->sr));
    float makeup = db_to_lin(c->makeup_db);

    for (uint32_t i = 0; i < n; i++) {
        float pl = fabsf(L[i]);
        float pr = fabsf(R[i]);
        float peak = pl > pr ? pl : pr;
        float db = 20.0f * log10f(peak + 1e-9f);
        float gain_db = 0.0f;
        if (db > (thresh - knee * 0.5f) && db < (thresh + knee * 0.5f)) {
            float x = db - thresh;
            gain_db = (db - thresh) * (slope - 1.0f) + (x*x/(2*knee))*(1.0f - slope);
        } else if (db >= (thresh + knee * 0.5f)) {
            gain_db = (db - thresh) * (slope - 1.0f);
        }
        float target_gain = powf(10.0f, gain_db / 20.0f);
        if (target_gain < c->env) c->env = att * c->env + (1.0f - att) * target_gain;
        else                      c->env = rel * c->env + (1.0f - rel) * target_gain;
        float g = c->env * makeup;
        L[i] *= g;
        R[i] *= g;
    }
}
