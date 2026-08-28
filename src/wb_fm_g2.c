/* wb_fm_g2.c — SIMD-accelerated FM render (G2).
 * Processes FM_VOICES as 4 batches of 4 via SSE2 polynomial sin.
 * Standalone file: includes its own compat stubs + g2_fm_simd.h.
 * NOTE: wb_fm_create/destroy/note are provided by wb_fm.o;
 * this file only defines wb_fm_render_g2.
 * fm_voice/fm_inst structs MUST match src/wb_fm.c EXACTLY. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "g2_fm_simd.h"

typedef float wb_sample;

typedef struct {
    double phase;
    double mphase;
    double freq;
    int    active;
    int    note;
    double env;
    int    releasing;
    uint8_t vel;
} fm_voice;

typedef struct {
    uint32_t sr;
    double ratio;
    double index;
    double env_a;
    double env_d;
    fm_voice v[16];
} fm_inst;

#define FM_VOICES 16
#define TWO_PI 6.2831853071795864769

static inline float hadd4(__m128 v) {
    __m128 lo = _mm_unpacklo_ps(v, v);
    __m128 hi = _mm_unpackhi_ps(v, v);
    __m128 sum = _mm_add_ps(lo, hi);
    __m128 shuf = _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2,3,0,1));
    return _mm_cvtss_f32(_mm_add_ss(sum, shuf));
}

void wb_fm_render_g2(void *unsafe, wb_sample *L, wb_sample *R, uint32_t n) {
    fm_inst *f = unsafe;
    fm_voice *v = f->v;
    double sr = (double)f->sr;
    double inv_sr = 1.0 / sr;
    double index = f->index;
    double env_a = f->env_a;

    for (uint32_t s = 0; s < n; s++) {
        float sum = 0.0f;
        for (int b = 0; b < FM_VOICES; b += 4) {
            float mph[4] = {0,0,0,0};
            float ph[4]  = {0,0,0,0};
            float en[4]  = {0,0,0,0};
            float vl[4]  = {0,0,0,0};

            for (int j = 0; j < 4; j++) {
                int idx = b + j;
                if (idx < FM_VOICES && v[idx].active) {
                    mph[j] = (float)v[idx].mphase;
                    ph[j]  = (float)v[idx].phase;
                    en[j]  = (float)v[idx].env;
                    vl[j]  = (float)v[idx].vel / 127.0f;
                }
            }
            __m128 mphase_vec = _mm_loadu_ps(mph);
            __m128 phase_vec  = _mm_loadu_ps(ph);
            __m128 env_vec    = _mm_loadu_ps(en);
            __m128 vel_vec    = _mm_loadu_ps(vl);

            __m128 s_vec = fm_simd_batch_4(mphase_vec, phase_vec, env_vec, index, NULL, NULL);

            __m128 term = _mm_mul_ps(s_vec, _mm_mul_ps(env_vec, vel_vec));
            sum += hadd4(term);

            for (int j = 0; j < 4; j++) {
                int idx = b + j;
                if (idx < FM_VOICES && v[idx].active) {
                    double mstep = TWO_PI * v[idx].freq * f->ratio * inv_sr;
                    double pstep = TWO_PI * v[idx].freq * inv_sr;
                    v[idx].mphase += mstep;
                    v[idx].phase  += pstep;
                    v[idx].env    += env_a;
                    if (v[idx].env > 1.0) v[idx].env = 1.0;
                }
            }
        }
        float out = sum * 0.35f;
        L[s] = out;
        R[s] = out;
    }
}
