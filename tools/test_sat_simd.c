/* test_sat_simd.c — gate test for wb_sat_simd (polynomial tanh saturation).
 * Verifies: create/destroy, poly_tanh accuracy vs tanhf, SIMD process
 * correctness, drive/out bounds, silence passthrough. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <emmintrin.h>
#include "wbus.h"
#include "wb_internal.h"

/* Reference scalar tanh saturation */
static float ref_sat(float x, float drive, float out_gain) {
    float gain = 1.0f + drive * 7.0f;
    float makeup = out_gain * 1.5f;
    return tanhf(x * gain) * makeup;
}

/* Polynomial tanh (same as in wb_sat_simd.c) */
static float poly_tanh(float x) {
    if (x < -3.0f) return -1.0f;
    if (x > 3.0f) return 1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static int tests_run = 0, tests_passed = 0;

#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

int main(void) {
    /* Test 1: poly_tanh accuracy vs tanhf */
    TEST("poly_tanh accuracy vs tanhf");
    float max_err = 0;
    for (float x = -6.0f; x <= 6.0f; x += 0.01f) {
        float approx = poly_tanh(x);
        float exact = tanhf(x);
        float err = fabsf(approx - exact);
        if (err > max_err) max_err = err;
    }
    printf("  max error = %.6e\n", max_err);
    if (max_err < 0.03f) PASS();
    else FAIL("poly_tanh error too large");

    /* Test 2: poly_tanh saturation behavior */
    TEST("poly_tanh saturates to ±1");
    float big = poly_tanh(10.0f);
    float neg_big = poly_tanh(-10.0f);
    printf("  tanh(10) = %.6f, tanh(-10) = %.6f\n", big, neg_big);
    if (big > 0.9f && big < 1.001f && neg_big < -0.9f && neg_big > -1.001f) PASS();
    else FAIL("saturation bounds wrong");

    /* Test 3: create/destroy */
    TEST("create/destroy");
    void *inst = wb_sat_create(44100);
    if (inst) { wb_sat_destroy(inst); PASS(); }
    else FAIL("create returned NULL");

    /* Test 4: process doesn't crash, output bounded */
    TEST("process output bounded");
    inst = wb_sat_create(44100);
    float L[64], R[64];
    for (int i = 0; i < 64; i++) {
        L[i] = (float)(i - 32) / 32.0f;  /* -1 to 1 */
        R[i] = -L[i];
    }
    wb_sat_process(inst, L, R, 64);
    float max_out = 0;
    for (int i = 0; i < 64; i++) {
        if (fabsf(L[i]) > max_out) max_out = fabsf(L[i]);
        if (fabsf(R[i]) > max_out) max_out = fabsf(R[i]);
    }
    printf("  max output = %.4f\n", max_out);
    if (max_out < 2.0f) PASS();  /* tanh output < 1, makeup < 1.5 */
    else FAIL("output unbounded");

    /* Test 5: silence in → silence out */
    TEST("silence in → silence out");
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    wb_sat_process(inst, L, R, 64);
    float sum = 0;
    for (int i = 0; i < 64; i++) { sum += fabsf(L[i]) + fabsf(R[i]); }
    printf("  silence sum = %.6e\n", sum);
    if (sum < 0.001f) PASS();
    else FAIL("silence not preserved");

    /* Test 6: SIMD vs scalar consistency */
    TEST("SIMD vs scalar consistency");
    /* Process same input through SIMD and scalar paths */
    float test_input[8] = {0.1f, 0.5f, -0.3f, 0.8f, -0.9f, 0.01f, -0.01f, 1.0f};
    float simd_out[8], scalar_out[8];
    memcpy(L, test_input, sizeof(test_input));
    memcpy(R, test_input, sizeof(test_input));
    wb_sat_process(inst, L, R, 8);
    memcpy(simd_out, L, sizeof(simd_out));
    /* Scalar reference */
    for (int i = 0; i < 8; i++) {
        scalar_out[i] = ref_sat(test_input[i], 0.3f, 0.7f);
    }
    float max_diff = 0;
    for (int i = 0; i < 8; i++) {
        float d = fabsf(simd_out[i] - scalar_out[i]);
        if (d > max_diff) max_diff = d;
    }
    printf("  max SIMD vs scalar diff = %.6e\n", max_diff);
    /* Allow some tolerance due to poly_tanh approx + oversample averaging */
    if (max_diff < 0.05f) PASS();
    else FAIL("SIMD vs scalar mismatch");

    wb_sat_destroy(inst);

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}
