/* test_macro_rack.c — gate test for wb_macro_rack (Ableton Instrument Rack style).
 * Verifies: create/destroy, add/remove units, macro control, parameter binding,
 * MIDI note processing, multiple macros, output finiteness.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "wbus.h"

/* Macro rack API declarations */
void *wb_rack_create(uint32_t sr, const char *name);
void  wb_rack_destroy(void *rack);
int   wb_rack_add_unit(void *rack, const char *unit_type);
int   wb_rack_remove_unit(void *rack, int index);
int   wb_rack_unit_count(const void *rack);
void  wb_rack_set_macro(void *rack, int macro_index, float value);
void  wb_rack_set_macro_name(void *rack, int macro_index, const char *name);
int   wb_rack_bind_param(void *rack, int macro_index, int unit_index,
                         int param_index, float min_val, float max_val);
void  wb_rack_process(void *rack, wb_sample *out, uint32_t frames);
void  wb_rack_note(void *rack, int note, int vel);
void  wb_rack_set_midi_in(void *rack, int enable);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

/* Compute peak amplitude of interleaved stereo buffer */
static float peak_stereo(const wb_sample *buf, uint32_t frames) {
    float peak = 0.0f;
    for (uint32_t i = 0; i < frames * 2; i++) {
        float a = fabsf(buf[i]);
        if (a > peak) peak = a;
    }
    return peak;
}

/* Check all samples are finite (no NaN/Inf) */
static int check_finite(const wb_sample *buf, uint32_t frames) {
    for (uint32_t i = 0; i < frames * 2; i++) {
        if (!isfinite(buf[i])) return 0;
    }
    return 1;
}

int main(void) {
    uint32_t sr = 44100;
    uint32_t frames = 2048;

    /* ---- Test 1: Create/destroy ---- */
    TEST("Create/destroy rack");
    {
        void *rack = wb_rack_create(sr, "Test Rack");
        if (rack) {
            wb_rack_destroy(rack);
            PASS();
        } else {
            FAIL("wb_rack_create returned NULL");
        }
    }

    /* ---- Test 2: Add units, verify count ---- */
    TEST("Add units, verify count");
    {
        void *rack = wb_rack_create(sr, "Test Rack");
        int idx_synth = wb_rack_add_unit(rack, "synth");
        int idx_filter = wb_rack_add_unit(rack, "filter");
        int idx_comp = wb_rack_add_unit(rack, "comp");
        int idx_delay = wb_rack_add_unit(rack, "delay");
        int idx_reverb = wb_rack_add_unit(rack, "reverb");

        if (idx_synth == 0 && idx_filter == 1 && idx_comp == 2 &&
            idx_delay == 3 && idx_reverb == 4 && wb_rack_unit_count(rack) == 5) {
            PASS();
        } else {
            FAIL("Unit indices or count mismatch");
        }
        wb_rack_destroy(rack);
    }

    /* ---- Test 3: Set macro, verify it affects processing ---- */
    TEST("Set macro affects processing");
    {
        void *rack = wb_rack_create(sr, "Test Rack");
        int si = wb_rack_add_unit(rack, "synth");
        wb_rack_bind_param(rack, 0, si, 0, 200.0f, 8000.0f); /* macro 0 -> synth filter cutoff */

        /* Set macro to 0 (min cutoff = 200) */
        wb_rack_set_macro(rack, 0, 0.0f);
        wb_rack_note(rack, 60, 127);

        wb_sample out0[4096];
        memset(out0, 0, sizeof(out0));
        wb_rack_process(rack, out0, frames);
        float peak0 = peak_stereo(out0, frames);

        /* Set macro to 1 (max cutoff = 8000) — more harmonics = different peak */
        wb_rack_set_macro(rack, 0, 1.0f);
        wb_rack_note(rack, 60, 127);

        wb_sample out1[4096];
        memset(out1, 0, sizeof(out1));
        wb_rack_process(rack, out1, frames);
        float peak1 = peak_stereo(out1, frames);

        /* Both should produce audio, and they should differ */
        if (peak0 > 0.001f && peak1 > 0.001f && fabsf(peak0 - peak1) > 0.0001f) {
            PASS();
        } else {
            printf("  peak0=%.4f peak1=%.4f\n", peak0, peak1);
            FAIL("Macro change did not affect output");
        }
        wb_rack_destroy(rack);
    }

    /* ---- Test 4: Bind macro to parameter ---- */
    TEST("Bind macro to parameter");
    {
        void *rack = wb_rack_create(sr, "Test Rack");
        int si = wb_rack_add_unit(rack, "synth");
        int ci = wb_rack_add_unit(rack, "comp");

        int b1 = wb_rack_bind_param(rack, 0, si, 0, 100.0f, 10000.0f);  /* cutoff */
        int b2 = wb_rack_bind_param(rack, 1, ci, 0, -30.0f, 0.0f);      /* threshold */

        if (b1 == 0 && b2 == 0) {
            PASS();
        } else {
            FAIL("Binding failed");
        }
        wb_rack_destroy(rack);
    }

    /* ---- Test 5: Process audio with MIDI note ---- */
    TEST("Process audio with MIDI note");
    {
        void *rack = wb_rack_create(sr, "Test Rack");
        wb_rack_add_unit(rack, "synth");
        wb_rack_add_unit(rack, "filter");
        wb_rack_add_unit(rack, "comp");
        wb_rack_add_unit(rack, "delay");
        wb_rack_add_unit(rack, "reverb");

        wb_rack_note(rack, 69, 100); /* A4, velocity 100 */

        wb_sample out[4096];
        memset(out, 0, sizeof(out));
        wb_rack_process(rack, out, frames);
        float peak = peak_stereo(out, frames);

        if (peak > 0.001f) {
            PASS();
        } else {
            printf("  peak=%.6f\n", peak);
            FAIL("No audio produced from synth note");
        }
        wb_rack_destroy(rack);
    }

    /* ---- Test 6: Remove unit ---- */
    TEST("Remove unit");
    {
        void *rack = wb_rack_create(sr, "Test Rack");
        wb_rack_add_unit(rack, "synth");
        wb_rack_add_unit(rack, "filter");
        wb_rack_add_unit(rack, "comp");

        int ret = wb_rack_remove_unit(rack, 1); /* remove filter */
        if (ret == 0 && wb_rack_unit_count(rack) == 2) {
            PASS();
        } else {
            FAIL("Remove unit failed");
        }
        wb_rack_destroy(rack);
    }

    /* ---- Test 7: Multiple macros ---- */
    TEST("Multiple macros");
    {
        void *rack = wb_rack_create(sr, "Test Rack");
        int si = wb_rack_add_unit(rack, "synth");
        int ci = wb_rack_add_unit(rack, "comp");
        int di = wb_rack_add_unit(rack, "delay");

        wb_rack_set_macro_name(rack, 0, "Cutoff");
        wb_rack_set_macro_name(rack, 1, "Threshold");
        wb_rack_set_macro_name(rack, 2, "Delay Mix");

        wb_rack_bind_param(rack, 0, si, 0, 200.0f, 12000.0f);
        wb_rack_bind_param(rack, 1, ci, 0, -24.0f, 0.0f);
        wb_rack_bind_param(rack, 2, di, 2, 0.0f, 1.0f);

        wb_rack_set_macro(rack, 0, 0.3f);
        wb_rack_set_macro(rack, 1, 0.7f);
        wb_rack_set_macro(rack, 2, 0.5f);

        wb_rack_note(rack, 60, 127);

        wb_sample out[4096];
        memset(out, 0, sizeof(out));
        wb_rack_process(rack, out, frames);
        float peak = peak_stereo(out, frames);

        if (peak > 0.001f) {
            PASS();
        } else {
            FAIL("Multiple macros did not produce audio");
        }
        wb_rack_destroy(rack);
    }

    /* ---- Test 8: Output finite (no NaN) ---- */
    /* ---- Test 8: Output finite (no NaN) ---- */
    TEST("Output finite (no NaN/Inf)");
    {
        void *rack = wb_rack_create(sr, "Test Rack");
        wb_rack_add_unit(rack, "synth");
        wb_rack_add_unit(rack, "filter");
        wb_rack_add_unit(rack, "comp");
        wb_rack_add_unit(rack, "delay");
        wb_rack_add_unit(rack, "reverb");

        /* Bind macros to extreme values */
        wb_rack_bind_param(rack, 0, 0, 0, 20.0f, 20000.0f);
        wb_rack_bind_param(rack, 1, 2, 0, -60.0f, 0.0f);
        wb_rack_set_macro(rack, 0, 0.0f);
        wb_rack_set_macro(rack, 1, 1.0f);

        wb_rack_note(rack, 24, 127); /* very low note */

        wb_sample out[4096];
        memset(out, 0, sizeof(out));
        wb_rack_process(rack, out, frames);

        if (check_finite(out, frames)) {
            PASS();
        } else {
            FAIL("Output contains NaN or Inf");
        }
        wb_rack_destroy(rack);
    }

    /* ---- Test 9: Invalid unit type rejected ---- */
    TEST("Invalid unit type rejected");
    {
        void *rack = wb_rack_create(sr, "Test Rack");
        int ret = wb_rack_add_unit(rack, "nonexistent");
        if (ret == -1 && wb_rack_unit_count(rack) == 0) {
            PASS();
        } else {
            FAIL("Invalid unit type was accepted");
        }
        wb_rack_destroy(rack);
    }

    /* ---- Test 10: MIDI enable/disable ---- */
    TEST("MIDI in enable/disable");
    {
        void *rack = wb_rack_create(sr, "Test Rack");
        wb_rack_set_midi_in(rack, 1);
        wb_rack_set_midi_in(rack, 0);
        /* Just verify it doesn't crash */
        PASS();
        wb_rack_destroy(rack);
    }

    /* ---- Summary ---- */
    printf("\n===== Macro Rack Tests: %d/%d passed =====\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}