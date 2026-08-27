/* wb_biquad_cascade_simd.c — SIMD biquad cascade (4 independent cascades in parallel).
 *
 * Two paths:
 * 1. Scalar PFE: partial fraction expansion for analysis/reference
 * 2. SIMD cascade4: 4 independent N-stage cascades in parallel (the fast path)
 *
 * G4 [R075]: 4× throughput for multi-stage filters (EQ, crossover, etc.).
 * Pure C11, SSE2 intrinsics. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <emmintrin.h>
#include "wbus_biquad_cascade.h"

/* ---- Scalar PFE path ---- */

static int solve_quadratic(float a, float b, float c, float *r0, float *r1) {
    if (fabsf(a) < 1e-12f) {
        if (fabsf(b) < 1e-12f) return 0;
        *r0 = -c / b; *r1 = *r0; return 1;
    }
    float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) { *r0 = -b/(2*a); *r1 = *r0; return -1; }
    float sq = sqrtf(disc);
    *r0 = (-b + sq)/(2*a); *r1 = (-b - sq)/(2*a); return 2;
}

static float poly_eval(const float *c, int deg, float x) {
    float r = 0, xn = 1;
    for (int i = 0; i <= deg; i++) { r += c[i]*xn; xn *= x; }
    return r;
}

static void poly_multiply(const float *a, int deg_a, const float *b, int deg_b, float *r) {
    int deg_r = deg_a + deg_b;
    memset(r, 0, (deg_r + 1) * sizeof(float));
    for (int i = 0; i <= deg_a; i++)
        for (int j = 0; j <= deg_b; j++)
            r[i+j] += a[i] * b[j];
}

int wb_biquad_cascade_init(wb_biquad_cascade *c,
                            const biquad_section_t *biquads, int n_biquads) {
    if (!c || !biquads || n_biquads < 1 || n_biquads > 8) return -1;
    memset(c, 0, sizeof(*c));

    /* Combined polynomials */
    int deg = 2 * n_biquads;
    float *b = calloc(deg + 1, sizeof(float));
    float *a = calloc(deg + 1, sizeof(float));
    float *tmp = calloc(deg + 1, sizeof(float));

    b[0] = biquads[0].b0; b[1] = biquads[0].b1; b[2] = biquads[0].b2;
    a[0] = 1.0f; a[1] = biquads[0].a1; a[2] = biquads[0].a2;
    int cur_deg = 2;

    for (int i = 1; i < n_biquads; i++) {
        float nb[3] = { biquads[i].b0, biquads[i].b1, biquads[i].b2 };
        float na[3] = { 1.0f, biquads[i].a1, biquads[i].a2 };
        poly_multiply(b, cur_deg, nb, 2, tmp); memcpy(b, tmp, (deg+1)*sizeof(float));
        poly_multiply(a, cur_deg, na, 2, tmp); memcpy(a, tmp, (deg+1)*sizeof(float));
        cur_deg += 2;
    }

    /* Find poles from each biquad factor */
    float poles_r[16], poles_i[16];
    int n_poles = 0;
    for (int i = 0; i < n_biquads; i++) {
        float pr, pi;
        int n = solve_quadratic(1.0f, biquads[i].a1, biquads[i].a2, &pr, &pi);
        if (n == -1) {
            float disc2 = biquads[i].a1*biquads[i].a1 - 4.0f*biquads[i].a2;
            float imag = (disc2 < 0) ? sqrtf(-disc2)/2.0f : 0.0f;
            poles_r[n_poles] = -biquads[i].a1/2.0f; poles_i[n_poles] = imag; n_poles++;
            poles_r[n_poles] = -biquads[i].a1/2.0f; poles_i[n_poles] = -imag; n_poles++;
        } else if (n >= 1) {
            poles_r[n_poles] = pr; poles_i[n_poles] = 0; n_poles++;
            if (n >= 2) { poles_r[n_poles] = pi; poles_i[n_poles] = 0; n_poles++; }
        }
    }

    /* Residues for real poles → first-order sections */
    for (int i = 0; i < n_poles && c->n_sections < WB_BIQUAD_CASCADE_MAX_SECTIONS; i++) {
        if (poles_i[i] != 0.0f) continue;
        float p = poles_r[i];
        float bp = poly_eval(b, deg, p);
        float ap = 0;
        for (int j = 1; j <= deg; j++) ap += (float)j * a[j] * powf(p, (float)(j-1));
        if (fabsf(ap) < 1e-12f) continue;
        c->sections[c->n_sections].gain = bp / ap;
        c->sections[c->n_sections].pole = p;
        c->sections[c->n_sections].state = 0;
        c->n_sections++;
    }

    if (c->n_sections == 0) {
        c->sections[0].gain = 1.0f; c->sections[0].pole = 0; c->sections[0].state = 0;
        c->n_sections = 1;
    }

    free(b); free(a); free(tmp);
    return 0;
}

float wb_biquad_cascade_process_scalar(wb_biquad_cascade *c, float x) {
    float y = 0;
    for (int i = 0; i < c->n_sections; i++) {
        wb_firstorder_section *s = &c->sections[i];
        float out = x + s->pole * s->state;
        s->state = out;
        y += s->gain * out;
    }
    return y;
}

void wb_biquad_cascade_reset(wb_biquad_cascade *c) {
    for (int i = 0; i < c->n_sections; i++) c->sections[i].state = 0;
}

/* ---- SIMD Path: 4 independent cascades in parallel ---- */

int wb_biquad_cascade4_init(wb_biquad_cascade4 *c4,
                             const biquad_section_t *biquads, int n_biquads) {
    if (!c4 || !biquads || n_biquads < 1 || n_biquads > WB_BIQUAD_CASCADE_MAX_STAGES)
        return -1;
    memset(c4, 0, sizeof(*c4));
    c4->n_stages = n_biquads;
    for (int s = 0; s < n_biquads; s++) {
        c4->b0[s] = _mm_set1_ps(biquads[s].b0);
        c4->b1[s] = _mm_set1_ps(biquads[s].b1);
        c4->b2[s] = _mm_set1_ps(biquads[s].b2);
        c4->a1[s] = _mm_set1_ps(biquads[s].a1);
        c4->a2[s] = _mm_set1_ps(biquads[s].a2);
    }
    return 0;
}

__m128 wb_biquad_cascade4_process(wb_biquad_cascade4 *c4, __m128 x) {
    __m128 y = x;
    for (int s = 0; s < c4->n_stages; s++) {
        __m128 new_y = _mm_add_ps(_mm_mul_ps(c4->b0[s], y), c4->s1[s]);
        c4->s1[s] = _mm_add_ps(
            _mm_sub_ps(_mm_mul_ps(c4->b1[s], y), _mm_mul_ps(c4->a1[s], new_y)),
            c4->s2[s]);
        c4->s2[s] = _mm_sub_ps(_mm_mul_ps(c4->b2[s], y), _mm_mul_ps(c4->a2[s], new_y));
        y = new_y;
    }
    return y;
}

void wb_biquad_cascade4_reset(wb_biquad_cascade4 *c4) {
    for (int s = 0; s < c4->n_stages; s++) {
        c4->s1[s] = _mm_setzero_ps();
        c4->s2[s] = _mm_setzero_ps();
    }
}
