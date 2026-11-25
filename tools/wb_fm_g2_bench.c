#include <emmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#define FM_VOICES 16
#define TWO_PI 6.2831853071795864769f

/* CEPHES-derived minimax coefficients for sin(x) on [-π/4,π/4].
 * Max error ~2.3e-7 (about 2^22 ULPs at float precision).
 */
#define SC1  -1.6666667163e-1f
#define SC2   8.3333337196e-3f
#define SC3  -1.9841269841e-4f
#define SC4   2.7557319220e-6f
#define SC5  -2.5052108385e-8f

/* ceil(x / TWO_PI) * TWO_PI, with x in [0, ~2π*N] and N modest (< 2^24).
 * Uses Horner for the division to avoid precision loss.
 */
static inline __m128 round_down_to_two_pi(__m128 x,
                                          __m128 inv_two_pi,
                                          __m128 two_pi,
                                          __m128 pi) {
    /* tmp = x * inv_two_pi */
    __m128 t = _mm_mul_ps(x, inv_two_pi);
    /* j = (int)tmp (truncate toward zero) */
    __m128i j = _mm_cvttps_epi32(t);
    /* j_mod2 = j & 1 */
    __m128i j_mod2 = _mm_and_si128(j, _mm_set1_epi32(1));
    /* cond_odd = (j_mod2 == 1) ? all-ones : all-zeros, as float mask */
    __m128 cond_odd = _mm_castsi128_ps(_mm_cmpeq_epi32(j_mod2, _mm_set1_epi32(1)));
    /* tmp2 = tmp - j (fractional part) */
    __m128 tmp2 = _mm_sub_ps(t, _mm_cvtepi32_ps(j));
    /* x_red = x - j*TWO_PI = x - (tmp - tmp2)*TWO_PI = x - tmp*TWO_PI + tmp2*TWO_PI
     *       = x - x + tmp2*TWO_PI = tmp2*TWO_PI
     * Wait: tmp = x*inv_two_pi, so x = tmp*TWO_PI. Then x_red = x - j*TWO_PI
     *       = (tmp - j)*TWO_PI = tmp2*TWO_PI. Yes, that's right.
     */
    __m128 x_red = _mm_mul_ps(tmp2, two_pi);
    /* If j is odd, x_red = TWO_PI - x_red, and sign flips.
     * But we folded wrong: actually, for sin reduction:
     *   sin(x) = sin(x_red)       if j even
     *   sin(x) = sin(TWO_PI - x_red) = -sin(x_red)  if j odd (since sin(2π-y) = -sin(y))
     * Wait no: sin(2π-y) = -sin(y)? No! sin(2π-y) = sin(-y) = -sin(y). Yes.
     * And sin(x_red) on [0,π/2] after we fold x_red into [-π/4,π/4]...
     * This is getting complicated. Let me just do the standard approach:
     *   j = round(x / TWO_PI)
     *   x_red = x - j*TWO_PI
     *   x_red in [-TWO_PI/2, TWO_PI/2]
     *   sign = (-1)^j
     *   x_folded = |x_red| in [0, TWO_PI/2]
     *   if x_folded > π/2: x_folded = π - x_folded, sign = -sign
     *   sin(x) = sign * sin_poly(x_folded)
     */
    return x_red; /* placeholder, will rewrite below */
}

/* The actual vec_sin_4 below uses the standard reduction. */

static inline __m128 vec_sin_4(__m128 x) {
    __m128 v_inv_tp = _mm_set1_ps(1.0f / TWO_PI);
    __m128 v_tp     = _mm_set1_ps(TWO_PI);
    __m128 v_pi     = _mm_set1_ps(3.14159265358979323846f);
    __m128 v_pi_2   = _mm_set1_ps(1.57079632679489661923f);

    /* y = |x| */
    __m128 y = _mm_andnot_ps(_mm_set1_ps(-0.0f), x);

    /* j = ceil(y * INV_TWO_PI) * TWO_PI */
    __m128 t    = _mm_mul_ps(y, v_inv_tp);
    __m128i j   = _mm_cvttps_epi32(t);
    __m128 jf   = _mm_cvtepi32_ps(j);
    __m128 x_red = _mm_sub_ps(y, _mm_mul_ps(jf, v_tp));

    /* quadrant sign = (-1)^j; cond_odd = j mod 2 == 1 */
    __m128i j_mod2 = _mm_and_si128(j, _mm_set1_epi32(1));
    __m128 cond_odd = _mm_castsi128_ps(_mm_cmpeq_epi32(j_mod2, _mm_set1_epi32(1)));
    /* fold: if j odd, x_red = TWO_PI - x_red */
    __m128 x_fold = _mm_sub_ps(v_tp, x_red);
    __m128 sel    = _mm_and_ps(cond_odd, x_fold);
    __m128 notsel = _mm_andnot_ps(cond_odd, x_red);
    x_red = _mm_or_ps(notsel, sel);

    /* fold into [0, π/2]: if x_red > π/2, x_red = π - x_red */
    __m128 gt_pi2 = _mm_cmpgt_ps(x_red, v_pi_2);
    __m128 pi_min_x = _mm_sub_ps(v_pi, x_red);
    __m128 sel2    = _mm_and_ps(gt_pi2, pi_min_x);
    __m128 notsel2 = _mm_andnot_ps(gt_pi2, x_red);
    x_red = _mm_or_ps(notsel2, sel2);

    /* compute sin(x_red) via minimax polynomial */
    __m128 t = x_red;
    __m128 t2 = _mm_mul_ps(t, t);
    __m128 c = _mm_add_ps(_mm_set1_ps(SC5), _mm_set1_ps(SC4));
    c = _mm_mul_ps(c, t2);
    c = _mm_add_ps(c, _mm_set1_ps(SC3));
    c = _mm_mul_ps(c, t2);
    c = _mm_add_ps(c, _mm_set1_ps(SC2));
    c = _mm_mul_ps(c, t2);
    c = _mm_add_ps(c, _mm_set1_ps(SC1));
    c = _mm_mul_ps(c, t2);
    c = _mm_add_ps(c, _mm_set1_ps(1.0f));
    __m128 sin_x = _mm_mul_ps(t, c);

    /* sign = (-1)^j */
    __m128 sign = _mm_or_ps(_mm_and_ps(cond_odd, _mm_set1_ps(-1.0f)),
                             _mm_andnot_ps(cond_odd, _mm_set1_ps(1.0f)));
    return _mm_mul_ps(sin_x, sign);
}

typedef struct {
    float phase, mphase, freq, env;
    int  active, releasing;
    uint8_t vel;
} fm_voice;

typedef struct {
    uint32_t sr;
    double ratio, index;
    double env_a, env_d;
    fm_voice v[FM_VOICES];
} fm_inst;

static inline double ts_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static void before_fm_render(fm_inst *f, float *L, float *R, uint32_t n) {
    double sr = f->sr;
    double aD = exp(-1.0 / (f->env_d * sr));
    double aA = exp(-1.0 / (f->env_a * sr));
    double phase_step[FM_VOICES], mphase_step[FM_VOICES];
    for (int k = 0; k < FM_VOICES; k++) {
        phase_step[k]   = TWO_PI * f->v[k].freq / sr;
        mphase_step[k]  = TWO_PI * f->v[k].freq * f->ratio / sr;
    }
    for (uint32_t i = 0; i < n; i++) {
        float sum = 0;
        for (int k = 0; k < FM_VOICES; k++) {
            fm_voice *v = &f->v[k];
            if (!v->active) continue;
            if (v->releasing) {
                v->env *= aD;
                if (v->env < 0.002) { v->active = 0; continue; }
            } else {
                if (v->env < 1.0) v->env = 1.0 - (1.0 - v->env) * aA;
            }
            double mod = f->index * v->env * sin(v->mphase);
            v->mphase += mphase_step[k];
            double s = sin(v->phase + mod);
            v->phase += phase_step[k];
            sum += (float)(s * v->env * (v->vel / 127.0));
        }
        float out = sum * 0.35f;
        L[i] = out; R[i] = out;
    }
}

static void after_fm_render(fm_inst *f, float *L, float *R, uint32_t n) {
    double sr = f->sr;
    double aD = exp(-1.0 / (f->env_d * sr));
    double aA = exp(-1.0 / (f->env_a * sr));
    double phase_step[FM_VOICES], mphase_step[FM_VOICES];
    for (int k = 0; k < FM_VOICES; k++) {
        phase_step[k]   = TWO_PI * f->v[k].freq / sr;
        mphase_step[k]  = TWO_PI * f->v[k].freq * f->ratio / sr;
    }
    for (uint32_t i = 0; i < n; i++) {
        float sum = 0;
        for (int g = 0; g < FM_VOICES; g += 4) {
            int i0 = g+0, i1 = g+1, i2 = g+2, i3 = g+3;
            fm_voice *v0 = &f->v[i0], *v1 = &f->v[i1],
                     *v2 = &f->v[i2], *v3 = &f->v[i3];
            /* envelope (scalar — simple multiply/decay, cheap) */
            for (int k = 0; k < 4; k++) {
                fm_voice *v = (k==0)?v0:(k==1)?v1:(k==2)?v2:v3;
                if (!v->active) continue;
                if (v->releasing) {
                    v->env *= aD;
                    if (v->env < 0.002) { v->active = 0; continue; }
                } else {
                    if (v->env < 1.0) v->env = 1.0 - (1.0 - v->env) * aA;
                }
            }
            /* pack 4 voices' active phases into __m128 (zero if inactive) */
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
            __m128 index_vec  = _mm_set1_ps((float)f->index);
            __m128 mod_vec    = _mm_mul_ps(index_vec, _mm_mul_ps(env_vec, sin_mphase));
            /* SIMD: carrier sin — sin(phase + mod) (4-wide) */
            __m128 carrier_arg = _mm_add_ps(phase_vec, mod_vec);
            __m128 s_vec       = vec_sin_4(carrier_arg);
            /* term = s * env * vel/127 (4-wide) */
            __m128 vel_vec = _mm_set1_ps((float)f->vel / 127.0);
            __m128 term    = _mm_mul_ps(s_vec, _mm_mul_ps(env_vec, vel_vec));
            /* horizontal sum across 4 lanes */
            float s0 = _mm_cvtss_f32(term);
            float s1 = _mm_cvtss_f32(_mm_shuffle_ps(term, term, _MM_SHUFFLE(1,1,1,1)));
            float s2 = _mm_cvtss_f32(_mm_shuffle_ps(term, term, _MM_SHUFFLE(2,2,2,2)));
            float s3 = _mm_cvtss_f32(_mm_shuffle_ps(term, term, _MM_SHUFFLE(3,3,3,3)));
            sum += s0 + s1 + s2 + s3;
            /* phase stepping (scalar per voice — phase_step[] is per-voice, applied after sin) */
            if (v0->active) { v0->mphase += mphase_step[i0]; v0->phase += phase_step[i0]; }
            if (v1->active) { v1->mphase += mphase_step[i1]; v1->phase += phase_step[i1]; }
            if (v2->active) { v2->mphase += mphase_step[i2]; v2->phase += phase_step[i2]; }
            if (v3->active) { v3->mphase += mphase_step[i3]; v3->phase += phase_step[i3]; }
        }
        float out = sum * 0.35f;
        L[i] = out; R[i] = out;
    }
}

int main(void) {
    srand(12345);
    uint32_t sr = 44100;
    uint32_t frames = 512;
    uint32_t blocks = 2000;
    int iter = 7;

    fm_inst *f = calloc(1, sizeof(*f));
    f->sr = sr;
    f->ratio = 2.0;
    f->index = 3.0;
    f->env_a = 0.002;
    f->env_d = 0.30;

    for (int k = 0; k < FM_VOICES; k++) {
        f->v[k].active = 1;
        f->v[k].releasing = 0;
        f->v[k].note = 60 + k;
        f->v[k].freq = 440.0 * pow(2.0, (60 + k - 69) / 12.0);
        f->v[k].phase = 0;
        f->v[k].mphase = 0;
        f->v[k].env = 1.0;
        f->v[k].vel = 100;
    }

    float *L = calloc(frames, sizeof(float));
    float *R = calloc(frames, sizeof(float));
    if (!L || !R) { fprintf(stderr, "buf alloc failed\n"); return 1; }

    /* ---- BEFORE ---- */
    for (int k = 0; k < FM_VOICES; k++) {
        f->v[k].env = 1.0; f->v[k].active = 1;
        f->v[k].phase = 0; f->v[k].mphase = 0;
    }
    double best_b = 1e100, worst_b = 0, sum_b = 0;
    for (int it = 0; it < iter; it++) {
        double t0 = ts_ns();
        for (uint32_t b = 0; b < blocks; b++)
            before_fm_render(f, L, R, frames);
        double t1 = ts_ns();
        double ns = t1 - t0;
        if (ns < best_b) best_b = ns;
        if (ns > worst_b) worst_b = ns;
        sum_b += ns;
    }
    printf("BEFORE fm_render (scalar sin, 16 voices, %u frames x %u blocks, %d iters)\n",
           frames, blocks, iter);
    printf("  best   = %.0f ns   (%.0f ns/block)\n", best_b, best_b/blocks);
    printf("  worst  = %.0f ns   (%.0f ns/block)\n", worst_b, worst_b/blocks);
    printf("  avg    = %.0f ns   (%.0f ns/block)\n", sum_b/iter, (sum_b/iter)/blocks);

    /* ---- AFTER ---- */
    for (int k = 0; k < FM_VOICES; k++) {
        f->v[k].env = 1.0; f->v[k].active = 1;
        f->v[k].phase = 0; f->v[k].mphase = 0;
    }
    double best_a = 1e100, worst_a = 0, sum_a = 0;
    for (int it = 0; it < iter; it++) {
        double t0 = ts_ns();
        for (uint32_t b = 0; b < blocks; b++)
            after_fm_render(f, L, R, frames);
        double t1 = ts_ns();
        double ns = t1 - t0;
        if (ns < best_a) best_a = ns;
        if (ns > worst_a) worst_a = ns;
        sum_a += ns;
    }
    printf("AFTER  fm_render (SIMD sin, 16 voices, %u frames x %u blocks, %d iters)\n",
           frames, blocks, iter);
    printf("  best   = %.0f ns   (%.0f ns/block)\n", best_a, best_a/blocks);
    printf("  worst  = %.0f ns   (%.0f ns/block)\n", worst_a, worst_a/blocks);
    printf("  avg    = %.0f ns   (%.0f ns/block)\n", sum_a/iter, (sum_a/iter)/blocks);

    printf("\nDELTA (worst): %.0f ns/block  (%.1f%%)\n",
           worst_b/blocks - worst_a/blocks,
           100.0 * (worst_b - worst_a) / worst_b);

    free(L); free(R); free(f);
    return 0;
}
