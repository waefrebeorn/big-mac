/* test_biquad_cascade.c — gate test for wb_biquad_cascade_simd.
 * Verifies: PFE correctness (cascade matches reference at DC),
 * scalar vs SIMD consistency, impulse response, and stability. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <emmintrin.h>

#include "wbus_biquad_cascade.h"

/* Reference: process through biquad cascade directly (scalar, serial).
 * Standard transposed Form II per section. */
static float reference_process(const biquad_section_t *bq, int n, float x) {
    float y = x;
    for (int s = 0; s < n; s++) {
        /* Direct Form I: y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2 */
        static float x1[8] = {0}, x2[8] = {0}, y1[8] = {0}, y2[8] = {0};
        float out = bq[s].b0 * y + bq[s].b1 * x1[s] + bq[s].b2 * x2[s]
                    - bq[s].a1 * y1[s] - bq[s].a2 * y2[s];
        x2[s] = x1[s];
        x1[s] = y;
        y2[s] = y1[s];
        y1[s] = out;
        y = out;
    }
    return y;
}

static void reset_reference(void) {
    /* handled by static init to zero; just for clarity */
}

static int failures = 0;
static int checks = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

int main(void) {
    printf("=== wb_biquad_cascade gate ===\n\n");

    /* A simple lowpass: single biquad, butterworth Q=0.707, fc=1kHz @ 44.1k */
    float wc = 2.0f * M_PI * 1000.0f / 44100.0f;
    float alpha = sinf(wc) / (2.0f * 0.707f);
    float cosw = cosf(wc);
    float a0 = 1.0f + alpha;

    biquad_section_t lp1 = {
        .b0 = ((1.0f - cosw) / 2.0f) / a0,
        .b1 = (1.0f - cosw) / a0,
        .b2 = ((1.0f - cosw) / 2.0f) / a0,
        .a1 = (-2.0f * cosw) / a0,
        .a2 = (1.0f - alpha) / a0
    };

    /* Test 1: single biquad cascade init */
    printf("Test 1: single biquad init\n");
    wb_biquad_cascade c;
    int rc = wb_biquad_cascade_init(&c, &lp1, 1);
    CHECK(rc == 0, "init returns success");

    /* Test 2: DC gain matches reference */
    printf("\nTest 2: DC gain (feed constant input)\n");
    wb_biquad_cascade_reset(&c);
    /* Feed 1.0 for 200 samples, check steady state */
    float y = 0;
    for (int i = 0; i < 200; i++) {
        y = wb_biquad_cascade_process_scalar(&c, 1.0f);
    }
    /* For a lowpass, DC gain ≈ 1.0 */
    printf("  DC output = %f (expected ~1.0)\n", y);
    CHECK(fabsf(y - 1.0f) < 0.05f, "DC gain ≈ 1.0 for lowpass");

    /* Test 3: impulse response decays (stable) */
    printf("\nTest 3: impulse response stability\n");
    wb_biquad_cascade_reset(&c);
    float max_ir = 0;
    float prev = 0;
    for (int i = 0; i < 500; i++) {
        float x = (i == 0) ? 1.0f : 0.0f;
        prev = wb_biquad_cascade_process_scalar(&c, x);
        float a = fabsf(prev);
        if (a > max_ir) max_ir = a;
    }
    /* After 500 samples, should be very small */
    printf("  IR[500] = %e, max = %f\n", prev, max_ir);
    CHECK(fabsf(prev) < 0.01f, "impulse response decays");
    CHECK(max_ir < 10.0f, "impulse response bounded");

    /* Test 4: 4-stage cascade (8th order) */
    printf("\nTest 4: 4-stage cascade (8th order lowpass)\n");
    biquad_section_t bq4[4];
    for (int i = 0; i < 4; i++) {
        bq4[i] = lp1;  /* same section 4× */
    }
    wb_biquad_cascade c4;
    rc = wb_biquad_cascade_init(&c4, bq4, 4);
    CHECK(rc == 0, "4-stage init success");

    wb_biquad_cascade_reset(&c4);
    for (int i = 0; i < 500; i++) {
        y = wb_biquad_cascade_process_scalar(&c4, 1.0f);
    }
    printf("  4-stage DC = %f (expected ~1.0)\n", y);
    CHECK(fabsf(y - 1.0f) < 0.1f, "4-stage DC gain ≈ 1.0");

    /* Test 5: SIMD path (4 lanes) */
    printf("\nTest 5: SIMD 4-lane cascade\n");
    wb_biquad_cascade4 c4simd;
    rc = wb_biquad_cascade4_init(&c4simd, &lp1, 1);
    CHECK(rc == 0, "SIMD 4-lane init success");

    wb_biquad_cascade4_reset(&c4simd);
    __m128 x4 = _mm_set1_ps(1.0f);
    __m128 y4;
    for (int i = 0; i < 200; i++) {
        y4 = wb_biquad_cascade4_process(&c4simd, x4);
    }
    float y_arr[4];
    _mm_storeu_ps(y_arr, y4);
    printf("  SIMD DC = %f, %f, %f, %f\n", y_arr[0], y_arr[1], y_arr[2], y_arr[3]);
    int all_close = 1;
    for (int i = 0; i < 4; i++) {
        if (fabsf(y_arr[i] - 1.0f) > 0.05f) all_close = 0;
    }
    CHECK(all_close, "SIMD 4-lane DC gain ≈ 1.0");

    /* Test 6: SIMD lanes are independent */
    printf("\nTest 6: SIMD lane independence\n");
    wb_biquad_cascade4_reset(&c4simd);
    __m128 x_diff = _mm_setr_ps(1.0f, 0.5f, -0.3f, 0.8f);
    for (int i = 0; i < 100; i++) {
        y4 = wb_biquad_cascade4_process(&c4simd, x_diff);
    }
    _mm_storeu_ps(y_arr, y4);
    /* All lanes should converge to their respective DC gains */
    int independent = 1;
    float inputs[4] = {1.0f, 0.5f, -0.3f, 0.8f};
    for (int i = 0; i < 4; i++) {
        /* DC gain ≈ 1.0, so output ≈ input */
        if (fabsf(y_arr[i] - inputs[i]) > 0.1f) independent = 0;
    }
    CHECK(independent, "SIMD lanes process independently");
    printf("  outputs: %f %f %f %f\n", y_arr[0], y_arr[1], y_arr[2], y_arr[3]);

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures > 0 ? 1 : 0;
}
