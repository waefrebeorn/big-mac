/* test_biquad4.c — verify SIMD biquad matches scalar biquad */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "wb_biquad_simd.h"

/* Scalar biquad (direct Form I, from wb_filter.c) */
typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; } scalar_biquad;

static inline float scalar_biquad_process(scalar_biquad *f, float x) {
    float y = f->b0*x + f->b1*f->x1 + f->b2*f->x2 - f->a1*f->y1 - f->a2*f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

int main(void) {
    /* Lowpass at 1000 Hz, Q=0.7, sr=44100 */
    float sr = 44100.0f;
    float w0 = 2.0f * (float)M_PI * 1000.0f / sr;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * 0.7f);
    float norm = 1.0f / (1.0f + alpha);
    float b0 = (1.0f - cw) * 0.5f * norm;
    float b1 = (1.0f - cw) * norm;
    float b2 = (1.0f - cw) * 0.5f * norm;
    float a1 = (-2.0f * cw) * norm;
    float a2 = (1.0f - alpha) * norm;

    printf("Coeffs: b0=%.6f b1=%.6f b2=%.6f a1=%.6f a2=%.6f\n", b0, b1, b2, a1, a2);

    /* Initialize SIMD biquad */
    wb_biquad4 bq4;
    wb_biquad4_init(&bq4, b0, b1, b2, a1, a2);

    /* Initialize 4 scalar biquads */
    scalar_biquad bq[4];
    for (int i = 0; i < 4; i++) {
        bq[i].b0 = b0; bq[i].b1 = b1; bq[i].b2 = b2;
        bq[i].a1 = a1; bq[i].a2 = a2;
        bq[i].x1 = 0; bq[i].x2 = 0; bq[i].y1 = 0; bq[i].y2 = 0;
    }

    /* Process 100 samples of impulse + random */
    float max_err = 0;
    float s1[4] = {0,0,0,0};
    float s2[4] = {0,0,0,0};

    for (int n = 0; n < 100; n++) {
        float x[4];
        for (int i = 0; i < 4; i++) {
            x[i] = (n == 0) ? 1.0f : ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        }

        /* Scalar */
        float ys[4];
        for (int i = 0; i < 4; i++) {
            ys[i] = scalar_biquad_process(&bq[i], x[i]);
        }

        /* SIMD */
        wb_biquad4_load_states(&bq4, s1[0], s1[1], s1[2], s1[3],
                                     s2[0], s2[1], s2[2], s2[3]);
        __m128 xv = _mm_loadu_ps(x);
        __m128 yv = wb_biquad4_process(&bq4, xv);
        wb_biquad4_store_states(&bq4, s1, s2);

        float yv_arr[4];
        _mm_storeu_ps(yv_arr, yv);

        for (int i = 0; i < 4; i++) {
            float err = fabsf(ys[i] - yv_arr[i]);
            if (err > max_err) max_err = err;
            if (n < 10) {
                printf("  n=%d lane=%d: scalar=%.6f simd=%.6f err=%.2e\n",
                       n, i, ys[i], yv_arr[i], err);
            }
        }
    }

    printf("\nMax error: %.2e\n", max_err);
    if (max_err < 1e-4f) {
        printf("PASS: SIMD biquad matches scalar (within float32 tolerance)\n");
        return 0;
    } else {
        printf("FAIL: error too large\n");
        return 1;
    }
}
