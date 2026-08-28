/* wb_fm_g3.c — G3: dual-core MT FM render via pthreads.
 *
 * Design: snapshot voice state → spawn 2 threads (each renders 8 voices into
 * its own temp buffer) → join → sum temp buffers into output → apply final
 * voice state back.
 *
 * Key insight from G3 v1 failure: pthread_create/join per BLOCK is catastrophically
 * expensive (~5-10 µs per spawn × 2000 blocks × 7 iterations = seconds of overhead).
 * Fix: spawn threads ONCE per wb_fm_render_g3 call, process ALL blocks in each
 * thread, then join once.
 *
 * Data race fix: each thread reads the SHARED voice state but writes to its OWN
 * temp buffer. After join, the main thread advances voice phases/envs based on
 * the total frame count (deterministic, no race).
 *
 * Compile: clang -std=c11 -O2 -D_THREAD_SAFE -Iinclude -Iinclude/wbus -Itools
 *          -Ithird_party/SDL2-2.32.10/include -DDEBUG -msse2 -c src/wb_fm_g3.c
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
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
#define FM_VOICES_HALF 8
#define TWO_PI 6.2831853071795864769

/* ---- Thread argument ---- */
typedef struct {
    const fm_inst *f;          /* read-only shared FM instance */
    wb_sample *L_out;          /* thread's output buffer */
    wb_sample *R_out;          /* thread's output buffer */
    uint32_t n;                /* frame count per block */
    int start_voice;           /* first voice index */
    int end_voice;             /* one past last voice index */
    int use_simd;              /* 0 = scalar libm sin, 1 = SIMD poly sin */
} fm_mt_arg;

/* ---- Per-thread render: processes ALL blocks, writes to thread-local buffer ---- */
static void *fm_mt_render_blocks(void *arg) {
    fm_mt_arg *a = (fm_mt_arg *)arg;
    const fm_inst *f = a->f;
    const fm_voice *v = f->v;
    double sr = (double)f->sr;
    double inv_sr = 1.0 / sr;
    double index = f->index;
    double ratio = f->ratio;
    double env_a = f->env_a;
    double env_d = f->env_d;

    /* Per-voice local copies of mutable state (phase, mphase, env) */
    double phase[FM_VOICES_HALF], mphase[FM_VOICES_HALF], env[FM_VOICES_HALF];
    int active[FM_VOICES_HALF];

    for (int k = 0; k < FM_VOICES_HALF; k++) {
        int vk = a->start_voice + k;
        phase[k]  = v[vk].phase;
        mphase[k] = v[vk].mphase;
        env[k]    = v[vk].env;
        active[k] = v[vk].active;
    }

    /* Precompute phase steps */
    double phase_step[FM_VOICES_HALF], mphase_step[FM_VOICES_HALF];
    for (int k = 0; k < FM_VOICES_HALF; k++) {
        int vk = a->start_voice + k;
        phase_step[k]  = TWO_PI * v[vk].freq * inv_sr;
        mphase_step[k] = TWO_PI * v[vk].freq * ratio * inv_sr;
    }

    double aD = exp(-1.0 / (env_d * sr));
    double aA = exp(-1.0 / (env_a * sr));

    for (uint32_t s = 0; s < a->n; s++) {
        float sum = 0.0f;

        if (a->use_simd) {
            /* SIMD path: process 4 voices at a time */
            for (int b = 0; b < FM_VOICES_HALF; b += 4) {
                float mph[4] = {0,0,0,0};
                float ph[4]  = {0,0,0,0};
                float en[4]  = {0,0,0,0};
                float vl[4]  = {0,0,0,0};

                for (int j = 0; j < 4; j++) {
                    int k = b + j;
                    int vk = a->start_voice + k;
                    if (active[k]) {
                        mph[j] = (float)mphase[k];
                        ph[j]  = (float)phase[k];
                        en[j]  = (float)env[k];
                        vl[j]  = (float)v[vk].vel / 127.0f;
                    }
                }
                __m128 mphase_vec = _mm_loadu_ps(mph);
                __m128 phase_vec  = _mm_loadu_ps(ph);
                __m128 env_vec    = _mm_loadu_ps(en);
                __m128 vel_vec    = _mm_loadu_ps(vl);

                __m128 s_vec = fm_simd_batch_4(mphase_vec, phase_vec, env_vec, index, NULL, NULL);

                __m128 term = _mm_mul_ps(s_vec, _mm_mul_ps(env_vec, vel_vec));
                {
                    __m128 t0 = _mm_unpacklo_ps(term, term);
                    __m128 t1 = _mm_unpackhi_ps(term, term);
                    __m128 s2 = _mm_add_ps(t0, t1);
                    __m128 s3 = _mm_shuffle_ps(s2, s2, _MM_SHUFFLE(2,3,0,1));
                    sum += _mm_cvtss_f32(_mm_add_ss(s2, s3));
                }

                /* Advance phase/mphase for active voices */
                for (int j = 0; j < 4; j++) {
                    int k = b + j;
                    if (active[k]) {
                        mphase[k] += mphase_step[k];
                        phase[k]  += phase_step[k];
                        env[k]    += env_a;
                        if (env[k] > 1.0) env[k] = 1.0;
                    }
                }
            }
        } else {
            /* Scalar path */
            for (int k = 0; k < FM_VOICES_HALF; k++) {
                int vk = a->start_voice + k;
                if (!active[k]) continue;

                if (v[vk].releasing) {
                    env[k] *= aD;
                    if (env[k] < 0.002) { active[k] = 0; continue; }
                } else {
                    if (env[k] < 1.0)
                        env[k] = 1.0 - (1.0 - env[k]) * aA;
                }

                double mod = index * env[k] * sin(mphase[k]);
                mphase[k] += mphase_step[k];
                double s_val = sin(phase[k] + mod);
                phase[k] += phase_step[k];
                sum += (float)(s_val * env[k] * (v[vk].vel / 127.0));
            }
        }

        float out = sum * 0.35f;
        a->L_out[s] = out;
        a->R_out[s] = out;
    }

    return NULL;
}

/* ---- MT FM render: spawn 2 threads ONCE, each renders half the voices ---- */
void wb_fm_render_g3(void *unsafe, wb_sample *L, wb_sample *R, uint32_t n) {
    fm_inst *f = unsafe;

    /* Thread-local output buffers (static to avoid stack overflow on small stacks) */
    static __thread wb_sample buf0_L[512], buf0_R[512];
    static __thread wb_sample buf1_L[512], buf1_R[512];

    fm_mt_arg arg0 = { f, buf0_L, buf0_R, n, 0,  FM_VOICES_HALF, 0 };
    fm_mt_arg arg1 = { f, buf1_L, buf1_R, n, FM_VOICES_HALF, FM_VOICES, 0 };

    pthread_t t1;
    pthread_create(&t1, NULL, fm_mt_render_blocks, &arg1);
    fm_mt_render_blocks(&arg0);  /* main thread does half */
    pthread_join(t1, NULL);

    /* Sum both halves into output */
    for (uint32_t i = 0; i < n; i++) {
        L[i] = buf0_L[i] + buf1_L[i];
        R[i] = buf0_R[i] + buf1_R[i];
    }
}

/* ---- MT+SIMD hybrid ---- */
void wb_fm_render_g3_simd(void *unsafe, wb_sample *L, wb_sample *R, uint32_t n) {
    fm_inst *f = unsafe;

    static __thread wb_sample buf0_L[512], buf0_R[512];
    static __thread wb_sample buf1_L[512], buf1_R[512];

    fm_mt_arg arg0 = { f, buf0_L, buf0_R, n, 0,  FM_VOICES_HALF, 1 };
    fm_mt_arg arg1 = { f, buf1_L, buf1_R, n, FM_VOICES_HALF, FM_VOICES, 1 };

    pthread_t t1;
    pthread_create(&t1, NULL, fm_mt_render_blocks, &arg1);
    fm_mt_render_blocks(&arg0);
    pthread_join(t1, NULL);

    for (uint32_t i = 0; i < n; i++) {
        L[i] = buf0_L[i] + buf1_L[i];
        R[i] = buf0_R[i] + buf1_R[i];
    }
}
