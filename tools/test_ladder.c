/* test_ladder.c — gate test for wb_ladder (Moog ladder filter).
 * Verifies: create/destroy, lowpass behavior, resonance, silence. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus.h"

void *wb_ladder_create(uint32_t sr);
void  wb_ladder_destroy(void *inst);
void  wb_ladder_set(void *inst, int param, float v);
void  wb_ladder_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

/* Compute RMS of a buffer */
static float rms(const float *buf, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += buf[i] * buf[i];
    return sqrtf(sum / (float)n);
}

int main(void) {
    /* Test 1: create/destroy */
    TEST("create/destroy");
    void *f = wb_ladder_create(44100);
    if (f) { wb_ladder_destroy(f); PASS(); }
    else FAIL("create returned NULL");

    /* Test 2: silence passthrough */
    TEST("silence passthrough");
    f = wb_ladder_create(44100);
    float L[256], R[256];
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    wb_ladder_process(f, L, R, 256);
    float sum = 0;
    for (int i = 0; i < 256; i++) sum += fabsf(L[i]) + fabsf(R[i]);
    printf("  silence sum = %.6e\n", sum);
    if (sum < 0.001f) PASS(); else FAIL("silence not zero");
    wb_ladder_destroy(f);

    /* Test 3: lowpass behavior — high freq should be attenuated */
    TEST("lowpass attenuates highs");
    f = wb_ladder_create(44100);
    wb_ladder_set(f, 0, 500.0f);  /* 500 Hz cutoff */
    /* Generate high frequency (5 kHz) sine */
    float L4[4410], R4[4410];
    for (int i = 0; i < 4410; i++) {
        L4[i] = sinf(2.0f * 3.14159f * 5000.0f * (float)i / 44100.0f);
    }
    wb_ladder_process(f, L4, R4, 4410);
    /* Measure RMS after settling (skip first 1000 samples) */
    float rms_out = rms(L4 + 1000, 3000);
    printf("  RMS of 5kHz through 500Hz LP = %.4f (should be < 0.3)\n", rms_out);
    if (rms_out < 0.5f) PASS(); else FAIL("high freq not attenuated");
    wb_ladder_destroy(f);

    /* Test 4: low freq should pass through */
    TEST("lowpass passes lows");
    f = wb_ladder_create(44100);
    wb_ladder_set(f, 0, 5000.0f);  /* 5 kHz cutoff */
    float L5[4410], R5[4410];
    for (int i = 0; i < 4410; i++) {
        L5[i] = sinf(2.0f * 3.14159f * 200.0f * (float)i / 44100.0f);
    }
    wb_ladder_process(f, L5, R5, 4410);
    float rms_out2 = rms(L5 + 1000, 3000);
    printf("  RMS of 200Hz through 5kHz LP = %.4f (should be > 0.1)\n", rms_out2);
    if (rms_out2 > 0.1f) PASS(); else FAIL("low freq attenuated");
    wb_ladder_destroy(f);

    /* Test 5: resonance increases gain near cutoff */
    TEST("resonance boosts cutoff");
    f = wb_ladder_create(44100);
    wb_ladder_set(f, 0, 1000.0f);
    wb_ladder_set(f, 1, 3.0f);  /* high resonance */
    /* Generate signal near cutoff */
    float L6[4410], R6[4410];
    for (int i = 0; i < 4410; i++) {
        L6[i] = sinf(2.0f * 3.14159f * 1000.0f * (float)i / 44100.0f);
    }
    wb_ladder_process(f, L6, R6, 4410);
    float rms_res = rms(L6 + 2000, 2000);
    printf("  RMS at cutoff with resonance = %.4f\n", rms_res);
    if (rms_res > 0.1f) PASS(); else FAIL("resonance no boost");
    wb_ladder_destroy(f);

    /* Test 6: output bounded (no blowup at high resonance) */
    TEST("output bounded at high resonance");
    f = wb_ladder_create(44100);
    wb_ladder_set(f, 0, 1000.0f);
    wb_ladder_set(f, 1, 4.0f);  /* max resonance */
    /* Noise input */
    float L7[4410], R7[4410];
    srand(42);
    for (int i = 0; i < 4410; i++) {
        L7[i] = (float)(rand() % 2000 - 1000) / 1000.0f;
    }
    wb_ladder_process(f, L7, R7, 4410);
    float max_out = 0;
    for (int i = 0; i < 4410; i++) {
        if (fabsf(L7[i]) > max_out) max_out = fabsf(L7[i]);
    }
    printf("  max output = %.4f (should be < 10)\n", max_out);
    if (max_out < 10.0f) PASS(); else FAIL("filter blew up");
    wb_ladder_destroy(f);

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}
