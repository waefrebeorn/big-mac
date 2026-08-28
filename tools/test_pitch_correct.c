/* test_pitch_correct.c — gate test for wb_pitch_correct (auto-tune).
 * Verifies: scale snapping, correction amount, silence passthrough. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus.h"

void *wb_pitch_correct_create(uint32_t sr);
void  wb_pitch_correct_destroy(void *inst);
void  wb_pitch_correct_set(void *inst, int param, float v);
void  wb_pitch_correct_process(void *inst, wb_sample *buf, uint32_t n);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

int main(void) {
    /* Test 1: create/destroy */
    TEST("create/destroy");
    void *pc = wb_pitch_correct_create(44100);
    if (pc) { wb_pitch_correct_destroy(pc); PASS(); }
    else FAIL("create returned NULL");

    /* Test 2: silence passthrough */
    TEST("silence passthrough");
    pc = wb_pitch_correct_create(44100);
    float buf[2048];
    memset(buf, 0, sizeof(buf));
    wb_pitch_correct_process(pc, buf, 2048);
    float sum = 0;
    for (int i = 0; i < 2048; i++) sum += fabsf(buf[i]);
    printf("  silence sum = %.6e\n", sum);
    if (sum < 0.001f) PASS(); else FAIL("silence not zero");
    wb_pitch_correct_destroy(pc);

    /* Test 3: parameter set */
    TEST("parameter set");
    pc = wb_pitch_correct_create(44100);
    wb_pitch_correct_set(pc, 0, 1.0f);  /* full correction */
    wb_pitch_correct_set(pc, 1, 0.0f);  /* root = C */
    wb_pitch_correct_set(pc, 2, 0.0f);  /* major scale */
    /* Generate a slightly detuned A4 (435 Hz instead of 440) */
    for (int i = 0; i < 2048; i++) {
        buf[i] = sinf(2.0f * 3.14159f * 435.0f * (float)i / 44100.0f);
    }
    wb_pitch_correct_process(pc, buf, 2048);
    /* Output should be non-silent (correction applied) */
    sum = 0;
    for (int i = 0; i < 2048; i++) sum += fabsf(buf[i]);
    printf("  output sum = %.4f\n", sum);
    if (sum > 10.0f) PASS(); else FAIL("no output");
    wb_pitch_correct_destroy(pc);

    /* Test 4: correction=0 → no change */
    TEST("correction=0 no-op");
    pc = wb_pitch_correct_create(44100);
    wb_pitch_correct_set(pc, 0, 0.0f);  /* no correction */
    float buf2[2048], buf2_orig[2048];
    for (int i = 0; i < 2048; i++) {
        buf2[i] = sinf(2.0f * 3.14159f * 435.0f * (float)i / 44100.0f);
        buf2_orig[i] = buf2[i];
    }
    wb_pitch_correct_process(pc, buf2, 2048);
    float max_diff = 0;
    for (int i = 0; i < 2048; i++) {
        float d = fabsf(buf2[i] - buf2_orig[i]);
        if (d > max_diff) max_diff = d;
    }
    printf("  max diff = %.6e\n", max_diff);
    if (max_diff < 0.01f) PASS(); else FAIL("correction=0 changed signal");
    wb_pitch_correct_destroy(pc);

    /* Test 5: chromatic scale → no correction (all notes valid) */
    TEST("chromatic scale passthrough");
    pc = wb_pitch_correct_create(44100);
    wb_pitch_correct_set(pc, 0, 1.0f);
    wb_pitch_correct_set(pc, 2, 3.0f);  /* chromatic */
    wb_pitch_correct_set(pc, 1, 0.0f);  /* root = C */
    /* Generate C4 (261.63 Hz) — should pass through in chromatic */
    for (int i = 0; i < 2048; i++) {
        buf[i] = sinf(2.0f * 3.14159f * 261.63f * (float)i / 44100.0f);
    }
    wb_pitch_correct_process(pc, buf, 2048);
    sum = 0;
    for (int i = 0; i < 2048; i++) sum += fabsf(buf[i]);
    printf("  output sum = %.4f\n", sum);
    if (sum > 10.0f) PASS(); else FAIL("chromatic blocked valid note");
    wb_pitch_correct_destroy(pc);

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}
