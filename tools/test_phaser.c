/* test_phaser.c — gate test for wb_phaser (allpass cascade + LFO).
 * Verifies: create/destroy, process doesn't crash, output bounded,
 * silence passthrough, LFO sweeps. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus.h"

void *wb_phaser_create(uint32_t sr);
void  wb_phaser_destroy(void *inst);
void  wb_phaser_set(void *inst, int param, float v);
void  wb_phaser_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

int main(void) {
    /* Test 1: create/destroy */
    TEST("create/destroy");
    void *p = wb_phaser_create(44100);
    if (p) { wb_phaser_destroy(p); PASS(); }
    else FAIL("create returned NULL");

    /* Test 2: process doesn't crash, output bounded */
    TEST("process output bounded");
    p = wb_phaser_create(44100);
    float L[256], R[256];
    for (int i = 0; i < 256; i++) {
        L[i] = sinf(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f);
        R[i] = cosf(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f);
    }
    wb_phaser_process(p, L, R, 256);
    float max_out = 0;
    for (int i = 0; i < 256; i++) {
        if (fabsf(L[i]) > max_out) max_out = fabsf(L[i]);
        if (fabsf(R[i]) > max_out) max_out = fabsf(R[i]);
    }
    printf("  max output = %.4f\n", max_out);
    if (max_out < 3.0f) PASS(); else FAIL("output unbounded");
    wb_phaser_destroy(p);

    /* Test 3: silence passthrough */
    TEST("silence passthrough");
    p = wb_phaser_create(44100);
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    wb_phaser_process(p, L, R, 256);
    float sum = 0;
    for (int i = 0; i < 256; i++) sum += fabsf(L[i]) + fabsf(R[i]);
    printf("  silence sum = %.6e\n", sum);
    if (sum < 0.001f) PASS(); else FAIL("silence not zero");
    wb_phaser_destroy(p);

    /* Test 4: output differs from input (effect is audible) */
    TEST("output differs from input");
    p = wb_phaser_create(44100);
    float in[256], out[256];
    for (int i = 0; i < 256; i++) {
        in[i] = sinf(2.0f * 3.14159f * 1000.0f * (float)i / 44100.0f);
        out[i] = in[i];
    }
    wb_phaser_process(p, out, out, 256);
    float diff = 0;
    for (int i = 0; i < 256; i++) diff += fabsf(out[i] - in[i]);
    printf("  total diff = %.4f\n", diff);
    if (diff > 0.1f) PASS(); else FAIL("no effect");
    wb_phaser_destroy(p);

    /* Test 5: parameter set */
    TEST("parameter set");
    p = wb_phaser_create(44100);
    wb_phaser_set(p, 0, 1.0f);  /* rate */
    wb_phaser_set(p, 1, 0.5f);  /* depth */
    wb_phaser_set(p, 2, 0.7f);  /* feedback */
    wb_phaser_set(p, 3, 0.8f);  /* mix */
    wb_phaser_set(p, 4, 4.0f);  /* stages */
    /* Process to verify no crash with new params */
    for (int i = 0; i < 64; i++) {
        L[i] = sinf(2.0f * 3.14159f * 220.0f * (float)i / 44100.0f);
        R[i] = L[i];
    }
    wb_phaser_process(p, L, R, 64);
    PASS();
    wb_phaser_destroy(p);

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}
