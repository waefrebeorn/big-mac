/* test_drum_machine.c — gate test for wb_drum_machine (TR-808/909).
 * Verifies: create/destroy, all voices trigger, output non-silent, no crash. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus.h"

void *wb_drum_machine_create(uint32_t sr);
void  wb_drum_machine_destroy(void *inst);
void  wb_drum_machine_note(void *inst, int note, int vel);
void  wb_drum_machine_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

/* Compute RMS */
static float rms(const float *buf, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += buf[i] * buf[i];
    return sqrtf(sum / (float)n);
}

int main(void) {
    void *dm = wb_drum_machine_create(44100);
    if (!dm) { printf("FAIL: create\n"); return 1; }

    wb_sample L[4410], R[4410];

    /* Test 1: bass drum */
    TEST("bass drum (BD)");
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    wb_drum_machine_note(dm, 36, 127);
    wb_drum_machine_render(dm, L, R, 4410);
    {
        float r = rms(L, 4410);
        printf("  RMS = %.4f\n", r);
        if (r > 0.01f) PASS(); else FAIL("BD silent");
    }

    /* Test 2: snare */
    TEST("snare (SD)");
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    wb_drum_machine_note(dm, 38, 127);
    wb_drum_machine_render(dm, L, R, 4410);
    {
        float r = rms(L, 4410);
        printf("  RMS = %.4f\n", r);
        if (r > 0.01f) PASS(); else FAIL("SD silent");
    }

    /* Test 3: all voices */
    TEST("all voices");
    int notes[] = {36, 38, 41, 45, 50, 39, 37, 56, 49, 46, 42};
    int n_notes = sizeof(notes) / sizeof(notes[0]);
    int all_ok = 1;
    for (int v = 0; v < n_notes; v++) {
        memset(L, 0, sizeof(L));
        memset(R, 0, sizeof(R));
        wb_drum_machine_note(dm, notes[v], 100);
        wb_drum_machine_render(dm, L, R, 4410);
        float r = rms(L, 4410);
        printf("  voice %d (note %d): RMS = %.4f\n", v, notes[v], r);
        if (r < 0.001f) all_ok = 0;
    }
    if (all_ok) PASS(); else FAIL("some voice silent");

    /* Test 4: silence when no notes */
    TEST("silence when idle");
    wb_drum_machine_destroy(dm);
    dm = wb_drum_machine_create(44100);  /* fresh instance */
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    wb_drum_machine_render(dm, L, R, 4410);
    {
        float r = rms(L, 4410);
        printf("  RMS = %.6e\n", r);
        if (r < 0.0001f) PASS(); else FAIL("not silent");
    }

    /* Test 5: multiple simultaneous hits */
    TEST("polyphony (multiple hits)");
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));
    wb_drum_machine_note(dm, 36, 127); /* BD */
    wb_drum_machine_note(dm, 42, 100); /* CH */
    wb_drum_machine_note(dm, 38, 110); /* SD */
    wb_drum_machine_render(dm, L, R, 4410);
    {
        float r = rms(L, 4410);
        printf("  RMS = %.4f\n", r);
        if (r > 0.01f) PASS(); else FAIL("polyphony silent");
    }

    wb_drum_machine_destroy(dm);

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}
