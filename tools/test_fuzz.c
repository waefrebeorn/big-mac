/* test_fuzz.c — gate test for wb_fuzz (Big Muff emulation).
 * Verifies: create/drive, distortion audible, tone control, silence. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus.h"

void *wb_fuzz_create(uint32_t sr);
void  wb_fuzz_destroy(void *inst);
void  wb_fuzz_set(void *inst, int param, float v);
void  wb_fuzz_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

static float rms(const float *buf, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += buf[i] * buf[i];
    return sqrtf(sum / (float)n);
}

int main(void) {
    void *f = wb_fuzz_create(44100);
    if (!f) { printf("FAIL: create\n"); return 1; }

    float L[4410], R[4410];
    int ok;

    /* Test 1: silence passthrough */
    TEST("silence passthrough");
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    wb_fuzz_process(f, L, R, 4410);
    {
        float sum = 0;
        for (int i = 0; i < 4410; i++) sum += fabsf(L[i]) + fabsf(R[i]);
        printf("  sum = %.6e\n", sum);
        if (sum < 0.001f) PASS(); else FAIL("silence not zero");
    }

    /* Test 2: distortion is audible (output differs from input) */
    TEST("distortion audible");
    wb_fuzz_set(f, 0, 0.8f);  /* high drive */
    wb_fuzz_set(f, 3, 1.0f);  /* full wet */
    for (int i = 0; i < 4410; i++) {
        L[i] = sinf(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f) * 0.5f;
        R[i] = L[i];
    }
    float orig_rms = rms(L, 4410);
    wb_fuzz_process(f, L, R, 4410);
    float fuzz_rms = rms(L, 4410);
    printf("  orig RMS = %.4f, fuzz RMS = %.4f\n", orig_rms, fuzz_rms);
    if (fuzz_rms > 0.05f && fabsf(fuzz_rms - orig_rms) > 0.01f) PASS();
    else FAIL("no distortion");

    /* Test 3: output bounded */
    TEST("output bounded");
    float max_out = 0;
    for (int i = 0; i < 4410; i++) {
        if (fabsf(L[i]) > max_out) max_out = fabsf(L[i]);
    }
    printf("  max output = %.4f\n", max_out);
    if (max_out < 10.0f) PASS(); else FAIL("output unbounded");

    /* Test 4: tone control changes output */
    TEST("tone control");
    wb_fuzz_set(f, 1, 0.0f);  /* dark */
    for (int i = 0; i < 4410; i++) {
        L[i] = sinf(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f) * 0.5f;
    }
    wb_fuzz_process(f, L, R, 4410);
    float dark_rms = rms(L, 1000);

    wb_fuzz_set(f, 1, 1.0f);  /* bright */
    wb_fuzz_destroy(f);
    f = wb_fuzz_create(44100);
    wb_fuzz_set(f, 0, 0.8f);
    wb_fuzz_set(f, 1, 1.0f);
    wb_fuzz_set(f, 3, 1.0f);
    for (int i = 0; i < 4410; i++) {
        L[i] = sinf(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f) * 0.5f;
    }
    wb_fuzz_process(f, L, R, 4410);
    float bright_rms = rms(L, 1000);
    printf("  dark RMS = %.4f, bright RMS = %.4f\n", dark_rms, bright_rms);
    if (fabsf(dark_rms - bright_rms) > 0.001f) PASS(); else FAIL("tone no effect");

    /* Test 5: drive increases distortion */
    TEST("drive control");
    wb_fuzz_destroy(f);
    f = wb_fuzz_create(44100);
    wb_fuzz_set(f, 3, 1.0f);
    wb_fuzz_set(f, 0, 0.1f);  /* low drive */
    for (int i = 0; i < 4410; i++) {
        L[i] = sinf(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f) * 0.5f;
    }
    wb_fuzz_process(f, L, R, 4410);
    float low_drive_rms = rms(L, 2000);

    wb_fuzz_destroy(f);
    f = wb_fuzz_create(44100);
    wb_fuzz_set(f, 3, 1.0f);
    wb_fuzz_set(f, 0, 0.9f);  /* high drive */
    for (int i = 0; i < 4410; i++) {
        L[i] = sinf(2.0f * 3.14159f * 440.0f * (float)i / 44100.0f) * 0.5f;
    }
    wb_fuzz_process(f, L, R, 4410);
    float high_drive_rms = rms(L, 2000);
    printf("  low drive RMS = %.4f, high drive RMS = %.4f\n", low_drive_rms, high_drive_rms);
    if (high_drive_rms != low_drive_rms) PASS(); else FAIL("drive no effect");

    wb_fuzz_destroy(f);

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}
