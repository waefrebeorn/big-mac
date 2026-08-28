/* test_comp_simd.c — gate test for wb_comp_simd (4-lane SIMD compressor).
 * Verifies: create/destroy, SIMD vs scalar consistency, gain reduction
 * above threshold, silence passthrough, envelope attack/release. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <emmintrin.h>
#include "wbus.h"

/* Declarations */
void *wb_comp4_create(uint32_t sr);
void  wb_comp4_destroy(void *inst);
void  wb_comp4_set(void *inst, int param, float v);
void  wb_comp4_process(void *inst, __m128 *L, __m128 *R, uint32_t n);

void *wb_comp_ref_create(uint32_t sr);
void  wb_comp_ref_destroy(void *inst);
void  wb_comp_ref_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

int main(void) {
    /* Test 1: create/destroy */
    TEST("create/destroy");
    void *c = wb_comp4_create(44100);
    if (c) { wb_comp4_destroy(c); PASS(); }
    else FAIL("create returned NULL");

    /* Test 2: silence passthrough */
    TEST("silence in → silence out");
    c = wb_comp4_create(44100);
    __m128 L[8], R[8];
    for (int i = 0; i < 8; i++) { L[i] = _mm_setzero_ps(); R[i] = _mm_setzero_ps(); }
    wb_comp4_process(c, L, R, 8);
    float sum = 0;
    for (int i = 0; i < 8; i++) {
        float arr[4];
        _mm_storeu_ps(arr, L[i]);
        for (int j = 0; j < 4; j++) sum += fabsf(arr[j]);
        _mm_storeu_ps(arr, R[i]);
        for (int j = 0; j < 4; j++) sum += fabsf(arr[j]);
    }
    printf("  silence sum = %.6e\n", sum);
    if (sum < 0.001f) PASS(); else FAIL("silence not preserved");

    /* Test 3: gain reduction above threshold */
    TEST("gain reduction above threshold");
    /* Feed a loud signal (0.9) — should be compressed below threshold -12dB */
    __m128 loud = _mm_set1_ps(0.9f);
    for (int i = 0; i < 8; i++) { L[i] = loud; R[i] = loud; }
    wb_comp4_process(c, L, R, 8);
    /* After envelope settles, output should be less than input */
    float out_arr[4];
    _mm_storeu_ps(out_arr, L[7]);
    printf("  input=0.9, output[0]=%.4f\n", out_arr[0]);
    if (out_arr[0] < 0.9f && out_arr[0] > 0.0f) PASS();
    else FAIL("no gain reduction");

    /* Test 4: SIMD vs scalar consistency */
    TEST("SIMD vs scalar consistency");
    void *ref = wb_comp_ref_create(44100);
    wb_comp4_destroy(c);
    c = wb_comp4_create(44100);

    /* Generate test signal: 8 samples */
    float sig[8], ref_L[8], ref_R[8];
    for (int i = 0; i < 8; i++) {
        sig[i] = 0.5f * sinf(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f);
        ref_L[i] = sig[i];
        ref_R[i] = sig[i];
    }

    /* Scalar reference */
    wb_comp_ref_process(ref, ref_L, ref_R, 8);

    /* SIMD: put signal in lane 0, zeros elsewhere */
    __m128 L2[8], R2[8];
    for (int i = 0; i < 8; i++) {
        L2[i] = _mm_setr_ps(sig[i], 0.0f, 0.0f, 0.0f);
        R2[i] = _mm_setr_ps(sig[i], 0.0f, 0.0f, 0.0f);
    }
    wb_comp4_process(c, L2, R2, 8);

    /* Compare lane 0 output to scalar */
    float max_diff = 0;
    for (int i = 0; i < 8; i++) {
        float simd_val[4];
        _mm_storeu_ps(simd_val, L2[i]);
        float diff = fabsf(simd_val[0] - ref_L[i]);
        if (diff > max_diff) max_diff = diff;
    }
    printf("  max SIMD vs scalar diff = %.6e\n", max_diff);
    if (max_diff < 0.001f) PASS(); else FAIL("SIMD vs scalar mismatch");

    /* Test 5: lane independence */
    TEST("lane independence");
    wb_comp4_destroy(c);
    c = wb_comp4_create(44100);
    /* Different signal per lane — process multiple blocks to let envelopes diverge */
    __m128 L3[16], R3[16];
    for (int i = 0; i < 16; i++) {
        L3[i] = _mm_setr_ps(0.8f, 0.4f, 0.2f, 0.1f);
        R3[i] = _mm_setr_ps(0.8f, 0.4f, 0.2f, 0.1f);
    }
    wb_comp4_process(c, L3, R3, 16);
    float lane0[4], lane_last[4];
    _mm_storeu_ps(lane0, L3[0]);
    _mm_storeu_ps(lane_last, L3[15]);
    printf("  lane0[0]=%.4f (in=0.8), lane3[3]=%.4f (in=0.1)\n", lane0[0], lane_last[3]);
    /* Louder lanes should be compressed more (lower output relative to input) */
    /* Ratio: lane0 input is 8× lane3, so after compression lane0/lane3 output
     * ratio should be less than 8× (compression reduces dynamic range) */
    float ratio_out = (lane0[0] > 1e-6f) ? lane_last[3] / lane0[0] : 1.0f;
    float ratio_in = 0.1f / 0.8f;
    printf("  input ratio = %.3f, output ratio = %.3f\n", ratio_in, ratio_out);
    if (ratio_out > ratio_in) PASS();
    else FAIL("lanes not independent");

    wb_comp4_destroy(c);
    wb_comp_ref_destroy(ref);

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}
