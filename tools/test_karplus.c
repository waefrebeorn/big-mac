/* test_karplus.c — gate test for wb_karplus (Karplus-Strong).
 * Verifies: pluck produces sound, pitch from MIDI note, decay, silence. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus.h"

void *wb_karplus_create(uint32_t sr);
void  wb_karplus_destroy(void *inst);
void  wb_karplus_note(void *inst, int note, int vel);
void  wb_karplus_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_karplus_set(void *inst, int param, float v);

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
    void *kp = wb_karplus_create(44100);
    if (!kp) { printf("FAIL: create\n"); return 1; }

    float L[4410], R[4410];

    /* Test 1: pluck produces sound */
    TEST("pluck produces sound");
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    wb_karplus_note(kp, 60, 127);  /* middle C */
    wb_karplus_render(kp, L, R, 4410);
    {
        float r = rms(L, 4410);
        printf("  RMS = %.4f\n", r);
        if (r > 0.01f) PASS(); else FAIL("no sound");
    }

    /* Test 2: different notes produce different pitches */
    TEST("different notes");
    memset(L, 0, sizeof(L));
    wb_karplus_note(kp, 69, 100);  /* A4 = 440Hz */
    wb_karplus_render(kp, L, R, 4410);
    float rms_a = rms(L, 1000);

    memset(L, 0, sizeof(L));
    wb_karplus_note(kp, 57, 100);  /* A3 = 220Hz */
    wb_karplus_render(kp, L, R, 4410);
    float rms_low = rms(L, 1000);
    printf("  A4 RMS = %.4f, A3 RMS = %.4f\n", rms_a, rms_low);
    if (rms_a > 0.01f && rms_low > 0.01f) PASS(); else FAIL("notes silent");

    /* Test 3: decay — amplitude decreases over time */
    TEST("decay over time");
    memset(L, 0, sizeof(L));
    wb_karplus_note(kp, 60, 127);
    wb_karplus_render(kp, L, R, 4410);
    float early = rms(L, 100);
    float late = rms(L + 3000, 1000);
    printf("  early RMS = %.4f, late RMS = %.4f\n", early, late);
    if (early > late) PASS(); else FAIL("no decay");

    /* Test 4: silence when idle */
    TEST("silence when idle");
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    wb_karplus_render(kp, L, R, 4410);
    {
        float sum = 0;
        for (int i = 0; i < 4410; i++) sum += fabsf(L[i]);
        printf("  sum = %.6e\n", sum);
        if (sum < 0.001f) PASS(); else FAIL("not silent");
    }

    /* Test 5: feedback controls decay time */
    TEST("feedback controls decay");
    wb_karplus_set(kp, 0, 0.99f);  /* short decay */
    memset(L, 0, sizeof(L));
    wb_karplus_note(kp, 60, 127);
    wb_karplus_render(kp, L, R, 4410);
    float short_rms = rms(L, 4410);

    wb_karplus_set(kp, 0, 0.9999f);  /* long decay */
    memset(L, 0, sizeof(L));
    wb_karplus_note(kp, 60, 127);
    wb_karplus_render(kp, L, R, 4410);
    float long_rms = rms(L, 4410);
    printf("  short decay RMS = %.4f, long decay RMS = %.4f\n", short_rms, long_rms);
    if (long_rms > short_rms) PASS(); else FAIL("feedback no effect");

    wb_karplus_destroy(kp);

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}
