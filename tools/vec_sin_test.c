/* vec_sin_4: 4-wide SSE2 sine for FM G2.
 *
 * Phases assumed in [0, 2π). Range-reduces via quadrant folding,
 * then applies CEPHES-derived minimax polynomial.
 *
 * Compile: clang -std=c11 -O2 -msse2 -I include -DTEST_VEC_SIN \
 *          tools/vec_sin_test.c -o /tmp/vec_sin_test -lm
 *
 * Run: /tmp/vec_sin_test
 * Expected: 16 active voices, 512-frame blocks, 2000 blocks, 7 iterations.
 * BEFORE: scalar sin, AFTER: SIMD sin via vec_sin_4.
 *
 * Output format:
 *   BEFORE fm_render (scalar sin, ...)
 *     best   = NNNNNNN ns   (NNNNNNN ns/block)
 *     worst  = NNNNNNN ns   (NNNNNNN ns/block)
 *     avg    = NNNNNNN ns   (NNNNNNN ns/block)
 *   AFTER  fm_render (SIMD sin, ...)
 *     best   = NNNNNNN ns   (NNNNNNN ns/block)
 *     worst  = NNNNNNN ns   (NNNNNNN ns/block)
 *     avg    = NNNNNNN ns   (NNNNNNN ns/block)
 */

#include <emmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define FM_VOICES 16
#define TWO_PI 6.2831853071795864769f

/* CEPHES-derived minimax polynomial coefficients for sin on [-π/2, π/2] */
#define S_C1  -0.1666666664f
#define S_C2   8.333333333e-3f
#define S_C3  -1.984126984e-4f
#define S_C4   2.755731922e-6f
#define S_C5  -2.505210838e-8f

/* ---- SIMD sin (4-wide) ---- */
static inline __m128 vec_sin_4(__m128 x) {
    __m128 v_inv_tp = _mm_set1_ps(1.0f / TWO_PI);
    __m128 v_tp     = _mm_set1_ps(TWO_PI);
    __m128 v_pi     = _mm_set1_ps(3.14159265358979323846f);
    __m128 v_pi_2   = _mm_set1_ps(1.57079632679489661923f);

    /* |x| via SSE2 bitwise */
    __m128 sign_mask = _mm_and_ps(x, _mm_set1_ps(-0.0f));
    __m128 y = _mm_xor_ps(x, sign_mask);

    /* j = (int)(y * INV_TWO_PI) */
    __m128i j = _mm_cvttps_epi32(_mm_mul_ps(y, v_inv_tp));
    __m128 x_red = _mm_sub_ps(x, _mm_mul_ps(_mm_cvtepi32_ps(j), v_tp));

    /* quadrant sign: (-1)^j */
    __m128i j_mod2 = _mm_and_si128(j, _mm_set1_epi32(1));
    __m128 cond_odd = _mm_castsi128_ps(_mm_cmpeq_epi32(j_mod2, _mm_set1_epi32(1)));

    /* fold: if j odd, x -> TWO_PI - x (quadrant 2-3 region) */
    __m128 x_fold = _mm_sub_ps(v_tp, x_red);
    __m128 m_andnot = _mm_andnot_ps(cond_odd, x_red);
    __m128 m_and    = _mm_and_ps(cond_odd, x_fold);
    x_red = _mm_or_ps(m_andnot, m_and);

    /* fold into [0, π/2] if > π/2 (quadrant 1-2 region) */
    __m128 gt_pi2 = _mm_cmpgt_ps(x_red, v_pi_2);
    __m128 pi_min_x = _mm_sub_ps(v_pi, x_red);
    __m128 g_andnot = _mm_andnot_ps(gt_pi2, x_red);
    __m128 g_and    = _mm_and_ps(gt_pi2, pi_min_x);
    x_red = _mm_or_ps(g_andnot, g_and);

    /* sin(t) ≈ t*(1 + t^2*(C1 + t^2*(C2 + t^2*(C3 + t^2*(C4 + t^2*C5))))) */
    __m128 t = x_red;
    __m128 t2 = _mm_mul_ps(t, t);
    __m128 c = _mm_add_ps(_mm_set1_ps(S_C5), _mm_set1_ps(S_C4));
    c = _mm_mul_ps(c, t2); c = _mm_add_ps(c, _mm_set1_ps(S_C3));
    c = _mm_mul_ps(c, t2); c = _mm_add_ps(c, _mm_set1_ps(S_C2));
    c = _mm_mul_ps(c, t2); c = _mm_add_ps(c, _mm_set1_ps(S_C1));
    c = _mm_mul_ps(c, t2); c = _mm_add_ps(c, _mm_set1_ps(1.0f));
    __m128 sin_val = _mm_mul_ps(t, c);

    /* sign = (-1)^j */
    __m128 sign_andnot = _mm_andnot_ps(cond_odd, _mm_set1_ps(1.0f));
    __m128 sign_and    = _mm_and_ps(cond_odd, _mm_set1_ps(-1.0f));
    __m128 sign = _mm_or_ps(sign_andnot, sign_and);
    return _mm_mul_ps(sin_val, sign);
}

/* ---- FM voice struct (matches wb_fm.c) ---- */
typedef struct {
    float phase;
    float mphase;
    float freq;
    int   active;
    int   note;
    float env;
    int   releasing;
    uint8_t vel;
} fm_voice;

typedef struct {
    unsigned sr;
    float ratio;
    float index;
    float env_a;
    float env_d;
    fm_voice v[FM_VOICES];
} fm_inst;

/* ---- BEFORE: scalar FM render ---- */
static void before_fm_render(fm_inst *f, float *L, float *R, unsigned n) {
    float aD = expf(-1.0f / (f->env_d * f->sr));
    float aA = expf(-1.0f / (f->env_a * f->sr));
    float phase_step[FM_VOICES], mphase_step[FM_VOICES];
    for (int k = 0; k < FM_VOICES; k++) {
        phase_step[k]   = TWO_PI * f->v[k].freq / f->sr;
        mphase_step[k]  = TWO_PI * f->v[k].freq * f->ratio / f->sr;
    }
    for (unsigned i = 0; i < n; i++) {
        float sum = 0;
        for (int k = 0; k < FM_VOICES; k++) {
            fm_voice *v = &f->v[k];
            if (!v->active) continue;
            if (v->releasing) {
                v->env *= aD;
                if (v->env < 0.002f) { v->active = 0; continue; }
            } else {
                if (v->env < 1.0f) v->env = 1.0f - (1.0f - v->env) * aA;
            }
            float mod = f->index * v->env * sinf(v->mphase);
            v->mphase += mphase_step[k];
            float s = sinf(v->phase + mod);
            v->phase += phase_step[k];
            sum += s * v->env * (v->vel / 127.0f);
        }
        float out = sum * 0.35f;
        L[i] = out;
        R[i] = out;
    }
}

/* ---- AFTER: SIMD FM render ---- */
static void after_fm_render(fm_inst *f, float *L, float *R, unsigned n) {
    float aD = expf(-1.0f / (f->env_d * f->sr));
    float aA = expf(-1.0f / (f->env_a * f->sr));
    float phase_step[FM_VOICES], mphase_step[FM_VOICES];
    for (int k = 0; k < FM_VOICES; k++) {
        phase_step[k]   = TWO_PI * f->v[k].freq / f->sr;
        mphase_step[k]  = TWO_PI * f->v[k].freq * f->ratio / f->sr;
    }
    for (unsigned i = 0; i < n; i++) {
        float sum = 0;
        for (int g = 0; g < FM_VOICES; g += 4) {
            int i0=g+0, i1=g+1, i2=g+2, i3=g+3;
            fm_voice *v0=&f->v[i0], *v1=&f->v[i1], *v2=&f->v[i2], *v3=&f->v[i3];
            /* envelope (scalar — simple multiply/decay, cheap) */
            for (int k=0; k<4; k++) {
                fm_voice *v = (k==0)?v0:(k==1)?v1:(k==2)?v2:v3;
                if (!v->active) continue;
                if (v->releasing) {
                    v->env *= aD;
                    if (v->env < 0.002f) { v->active = 0; continue; }
                } else {
                    if (v->env < 1.0f) v->env = 1.0f - (1.0f - v->env) * aA;
                }
            }
            /* Pack active phases for SIMD */
            float ma[4]={0,0,0,0}, pa[4]={0,0,0,0}, ea[4]={0,0,0,0};
            if (v0->active) ma[0]=v0->mphase, pa[0]=v0->phase, ea[0]=v0->env;
            if (v1->active) ma[1]=v1->mphase, pa[1]=v1->phase, ea[1]=v1->env;
            if (v2->active) ma[2]=v2->mphase, pa[2]=v2->phase, ea[2]=v2->env;
            if (v3->active) ma[3]=v3->mphase, pa[3]=v3->phase, ea[3]=v3->env;
            __m128 mphase_vec = _mm_loadu_ps(ma);
            __m128 phase_vec  = _mm_loadu_ps(pa);
            __m128 env_vec    = _mm_loadu_ps(ea);
            /* SIMD: modulator sin (4-wide) */
            __m128 sin_mphase = vec_sin_4(mphase_vec);
            __m128 index_vec  = _mm_set1_ps(f->index);
            __m128 mod_vec    = _mm_mul_ps(index_vec, _mm_mul_ps(env_vec, sin_mphase));
            /* SIMD: carrier sin — sin(phase + mod) (4-wide) */
            __m128 carrier_arg = _mm_add_ps(phase_vec, mod_vec);
            __m128 s_vec       = vec_sin_4(carrier_arg);
            /* term = s * env * vel/127 (4-wide) */
            __m128 vel_vec = _mm_set1_ps(f->vel / 127.0f);
            __m128 term    = _mm_mul_ps(s_vec, _mm_mul_ps(env_vec, vel_vec));
            /* horizontal sum across 4 lanes */
            float s0 = _mm_cvtss_f32(term);
            float s1 = _mm_cvtss_f32(_mm_shuffle_ps(term, term, _MM_SHUFFLE(1,1,1,1)));
            float s2 = _mm_cvtss_f32(_mm_shuffle_ps(term, term, _MM_SHUFFLE(2,2,2,2)));
            float s3 = _mm_cvtss_f32(_mm_shuffle_ps(term, term, _MM_SHUFFLE(3,3,3,3)));
            sum += s0 + s1 + s2 + s3;
            /* Phase stepping (scalar per voice) */
            if (v0->active) { v0->mphase += mphase_step[i0]; v0->phase += phase_step[i0]; }
            if (v1->active) { v1->mphase += mphase_step[i1]; v1->phase += phase_step[i1]; }
            if (v2->active) { v2->mphase += mphase_step[i2]; v2->phase += phase_step[i2]; }
            if (v3->active) { v3->mphase += mphase_step[i3]; v3->phase += phase_step[i3]; }
        }
        float out = sum * 0.35f;
        L[i] = out;
        R[i]= out;
    }
}

/* ---- main: before/after measurement ---- */
static double ts_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

int main(void) {
    srand(12345);
    unsigned sr = 44100;
    unsigned frames = 512;
    unsigned blocks = 2000;
    int iter = 7;

    fm_inst *f = calloc(1, sizeof(*f));
    f->sr = sr;
    f->ratio = 2.0f;
    f->index = 3.0f;
    f->env_a = 0.002f;
    f->env_d = 0.30f;

    /* 16 active FM voices, mid-range frequencies (A3..A4 spread) */
    for (int k = 0; k < FM_VOICES; k++) {
        f->v[k].active = 1;
        f->v[k].releasing = 0;
        f->v[k].note = 60 + k;
        f->v[k].freq = 440.0f * powf(2.0f, (60 + k - 69) / 12.0f);
        f->v[k].phase = 0;
        f->v[k].mphase = 0;
        f->v[k].env = 1.0f;
        f->v[k].vel = 100;
    }

    float *L = (float *)calloc(frames, sizeof(float));
    float *R = (float *)calloc(frames, sizeof(float));
    if (!L || !R) { fprintf(stderr, "buf alloc failed\n"); return 1; }

    /* ---- BEFORE (scalar) ---- */
    {
        for (int k = 0; k < FM_VOICES; k++) {
            f->v[k].env = 1.0f; f->v[k].active = 1; f->v[k].phase = 0; f->v[k].mphase = 0;
        }
        double best=1e100, worst=0, sum=0;
        for (int it=0; it<iter; it++) {
            double t0 = ts_ns();
            for (unsigned b=0; b<blocks; b++) before_fm_render(f, L, R, frames);
            double t1 = ts_ns();
            double ns = t1 - t0;
            if (ns<best) best=ns;
            if (ns>worst) worst=ns;
            sum += ns;
        }
        double avg = sum/iter;
        printf("BEFORE fm_render (scalar sin, 16 voices, %u frames x %u blocks, %d iters)\n", frames, blocks, iter);
        printf("  best   = %.0f ns   (%.0f ns/block)\n", best, best/blocks);
        printf("  worst  = %.0f ns   (%.0f ns/block)\n", worst, worst/blocks);
        printf("  avg    = %.0f ns   (%.0f ns/block)\n", avg, avg/blocks);
    }

    /* ---- AFTER (SIMD) ---- */
    {
        for (int k = 0; k < FM_VOICES; k++) {
            f->v[k].env = 1.0f; f->v[k].active = 1; f->v[k].phase = 0; f->v[k].mphase = 0;
        }
        double best=1e100, worst=0, sum=0;
        for (int it=0; it<iter; it++) {
            double t0 = ts_ns();
            for (unsigned b=0; b<blocks; b++) after_fm_render(f, L, R, frames);
            double t1 = ts_ns();
            double ns = t1 - t0;
            if (ns<best) best=ns;
            if (ns>worst) worst=ns;
            sum += ns;
        }
        double avg = sum/iter;
        printf("AFTER  fm_render (SIMD sin, 16 voices, %u frames x %u blocks, %d iters)\n", frames, blocks, iter);
        printf("  best   = %.0f ns   (%.0f ns/block)\n", best, best/blocks);
        printf("  worst  = %.0f ns   (%.0f ns/block)\n", worst, worst/blocks);
        printf("  avg    = %.0f ns   (%.0f ns/block)\n", avg, avg/blocks);
    }

    free(L); free(R);
    free(f);
    return 0;
}
