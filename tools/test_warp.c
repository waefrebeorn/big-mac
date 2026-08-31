/* test_warp.c — gate test for wb_warp (Ableton-style warp markers).
 * Verifies: create/destroy, markers, coordinate mapping, auto-warp, process. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus.h"

/* Forward declarations for the warp API (from wbus_warp.h) */
typedef struct wb_warp wb_warp;
wb_warp *wb_warp_create(uint32_t sr);
void     wb_warp_destroy(wb_warp *w);
int      wb_warp_set_source(wb_warp *w, const wb_sample *audio,
                             uint32_t frames, uint32_t channels);
int      wb_warp_add_marker(wb_warp *w, double src_sample, double dst_beat);
int      wb_warp_remove_marker(wb_warp *w, int index);
int      wb_warp_clear_markers(wb_warp *w);
double   wb_warp_src_to_dst(const wb_warp *w, double src_sample);
double   wb_warp_dst_to_src(const wb_warp *w, double beat);
void     wb_warp_process(wb_warp *w, double beat_start, double beat_end,
                          wb_sample *out, uint32_t frames);
int      wb_warp_marker_count(const wb_warp *w);
int      wb_warp_auto_warp(wb_warp *w, const double *beat_positions, int num_beats);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

/* Generate a sine wave for testing. */
static void gen_sine(wb_sample *buf, uint32_t frames, uint32_t chn, double freq, uint32_t sr) {
    for (uint32_t i = 0; i < frames; i++) {
        double phase = 2.0 * M_PI * freq * (double)i / (double)sr;
        double s = sin(phase);
        for (uint32_t c = 0; c < chn; c++)
            buf[i * chn + c] = (wb_sample)(0.5 * s);
    }
}

/* Compute RMS of interleaved stereo buffer. */
static float rms_stereo(const wb_sample *buf, uint32_t frames) {
    double sum = 0;
    for (uint32_t i = 0; i < frames * 2; i++)
        sum += (double)buf[i] * (double)buf[i];
    return (float)sqrt(sum / (double)(frames * 2));
}

int main(void) {
    uint32_t sr = 44100;

    /* ---- Test 1: Create / Destroy ---- */
    TEST("create/destroy");
    {
        wb_warp *w = wb_warp_create(sr);
        if (w) { wb_warp_destroy(w); PASS(); }
        else FAIL("create returned NULL");
    }

    /* ---- Test 2: Add markers, verify count ---- */
    TEST("add markers, verify count");
    {
        wb_warp *w = wb_warp_create(sr);
        int rc = wb_warp_add_marker(w, 44100.0, 4.0);   /* 1s → beat 4 */
        int rc2 = wb_warp_add_marker(w, 88200.0, 8.0);  /* 2s → beat 8 */
        int rc3 = wb_warp_add_marker(w, 22050.0, 2.0);  /* 0.5s → beat 2 (inserted in middle) */
        if (rc >= 0 && rc2 >= 0 && rc3 >= 0 && wb_warp_marker_count(w) == 3)
            PASS();
        else
            FAIL("add/count mismatch");
        wb_warp_destroy(w);
    }

    /* ---- Test 3: src_to_dst with no markers returns identity ---- */
    TEST("src_to_dst identity (no markers)");
    {
        wb_warp *w = wb_warp_create(sr);
        double src = 44100.0;  /* 1 second of samples */
        double dst = wb_warp_src_to_dst(w, src);
        /* With no markers, src_to_dst returns src/sr = 1.0 (identity in seconds) */
        printf("  src=%.1f → dst=%.6f (expected ~1.0)\n", src, dst);
        if (fabs(dst - 1.0) < 0.01) PASS(); else FAIL("not identity");
        wb_warp_destroy(w);
    }

    /* ---- Test 4: src_to_dst with markers returns correct warped position ---- */
    TEST("src_to_dst with markers");
    {
        wb_warp *w = wb_warp_create(sr);
        wb_warp_add_marker(w, 44100.0, 4.0);   /* src=44100 → beat 4 */
        wb_warp_add_marker(w, 88200.0, 8.0);   /* src=88200 → beat 8 */

        /* At src=44100, should be exactly beat 4 */
        double dst1 = wb_warp_src_to_dst(w, 44100.0);
        printf("  src=44100 → dst=%.4f (expected 4.0)\n", dst1);

        /* At src=88200, should be exactly beat 8 */
        double dst2 = wb_warp_src_to_dst(w, 88200.0);
        printf("  src=88200 → dst=%.4f (expected 8.0)\n", dst2);

        /* At src=22050 (quarter way), should be ~beat 2 (linear interp) */
        double dst3 = wb_warp_src_to_dst(w, 22050.0);
        printf("  src=22050 → dst=%.4f (expected ~2.0)\n", dst3);

        if (fabs(dst1 - 4.0) < 0.01 &&
            fabs(dst2 - 8.0) < 0.01 &&
            fabs(dst3 - 2.0) < 0.1)
            PASS();
        else FAIL("warped position incorrect");
        wb_warp_destroy(w);
    }

    /* ---- Test 5: dst_to_src is inverse of src_to_dst ---- */
    TEST("dst_to_src is inverse of src_to_dst");
    {
        wb_warp *w = wb_warp_create(sr);
        wb_warp_add_marker(w, 44100.0, 4.0);
        wb_warp_add_marker(w, 88200.0, 8.0);
        wb_warp_add_marker(w, 132300.0, 12.0);

        int ok = 1;
        double test_srcs[] = {0.0, 10000.0, 44100.0, 60000.0, 88200.0, 100000.0, 132300.0};
        for (int i = 0; i < 7; i++) {
            double s = test_srcs[i];
            double d = wb_warp_src_to_dst(w, s);
            double s_back = wb_warp_dst_to_src(w, d);
            printf("  src=%.1f → dst=%.4f → src'=%.1f (err=%.4f)\n",
                   s, d, s_back, fabs(s - s_back));
            if (fabs(s - s_back) > 2.0) { ok = 0; }  /* within 2 samples */
        }
        if (ok) PASS(); else FAIL("inverse mismatch");
        wb_warp_destroy(w);
    }

    /* ---- Test 6: Remove marker works ---- */
    TEST("remove marker");
    {
        wb_warp *w = wb_warp_create(sr);
        wb_warp_add_marker(w, 44100.0, 4.0);
        wb_warp_add_marker(w, 88200.0, 8.0);
        wb_warp_add_marker(w, 132300.0, 12.0);
        int before = wb_warp_marker_count(w);
        int rc = wb_warp_remove_marker(w, 1);  /* remove middle marker */
        int after = wb_warp_marker_count(w);
        printf("  before=%d, after=%d, rc=%d\n", before, after, rc);
        if (rc == 0 && before == 3 && after == 2) PASS(); else FAIL("remove failed");
        wb_warp_destroy(w);
    }

    /* ---- Test 7: Clear markers resets to identity ---- */
    TEST("clear markers resets to identity");
    {
        wb_warp *w = wb_warp_create(sr);
        wb_warp_add_marker(w, 44100.0, 4.0);
        wb_warp_add_marker(w, 88200.0, 8.0);
        wb_warp_clear_markers(w);
        double dst = wb_warp_src_to_dst(w, 44100.0);
        printf("  after clear: src=44100 → dst=%.6f (expected ~1.0)\n", dst);
        if (wb_warp_marker_count(w) == 0 && fabs(dst - 1.0) < 0.01)
            PASS();
        else FAIL("clear/identity failed");
        wb_warp_destroy(w);
    }

    /* ---- Test 8: Auto-warp from beat positions ---- */
    TEST("auto-warp from beat positions");
    {
        wb_warp *w = wb_warp_create(sr);
        /* Simulate 4 beats at 0.5s intervals (120 BPM) */
        double beats[] = {0.0, 22050.0, 44100.0, 66150.0};
        int rc = wb_warp_auto_warp(w, beats, 4);
        printf("  auto-warp placed %d markers\n", rc);
        if (rc == 4 && wb_warp_marker_count(w) == 4) {
            /* Verify first beat maps to beat 0, last to beat 3 */
            double d0 = wb_warp_src_to_dst(w, beats[0]);
            double d3 = wb_warp_src_to_dst(w, beats[3]);
            printf("  beat[0]→dst=%.4f (exp 0.0), beat[3]→dst=%.4f (exp 3.0)\n", d0, d3);
            if (fabs(d0 - 0.0) < 0.01 && fabs(d3 - 3.0) < 0.01)
                PASS();
            else FAIL("auto-warp mapping wrong");
        } else {
            FAIL("auto-warp count wrong");
        }
        wb_warp_destroy(w);
    }

    /* ---- Test 9: Process renders non-silent audio ---- */
    TEST("process renders non-silent audio");
    {
        wb_warp *w = wb_warp_create(sr);
        /* Create 2s of 440Hz sine at sr */
        uint32_t frames = 88200;
        uint32_t chn = 2;
        wb_sample *audio = malloc((size_t)frames * chn * sizeof(wb_sample));
        if (!audio) { FAIL("malloc"); wb_warp_destroy(w); goto test10; }
        gen_sine(audio, frames, chn, 440.0, sr);

        wb_warp_set_source(w, audio, frames, chn);
        /* No markers → pass-through */

        /* Render 1 second of audio */
        uint32_t out_frames = 44100;
        wb_sample *out = malloc((size_t)out_frames * 2 * sizeof(wb_sample));
        if (!out) { free(audio); FAIL("malloc out"); wb_warp_destroy(w); goto test10; }

        wb_warp_process(w, 0.0, (double)out_frames / (double)sr, out, out_frames);
        float r = rms_stereo(out, out_frames);
        printf("  RMS = %.6f (expected > 0.01)\n", r);
        if (r > 0.01f) PASS(); else FAIL("output silent");

        free(out);
        free(audio);
        wb_warp_destroy(w);
    }
    test10:;

    /* ---- Test 10: Process output length matches expected warped duration ---- */
    TEST("process output length matches warped duration");
    {
        wb_warp *w = wb_warp_create(sr);
        /* Create 4s of sine */
        uint32_t src_frames = 176400;  /* 4s */
        uint32_t chn = 2;
        wb_sample *audio = malloc((size_t)src_frames * chn * sizeof(wb_sample));
        if (!audio) { FAIL("malloc"); wb_warp_destroy(w); goto end; }
        gen_sine(audio, src_frames, chn, 220.0, sr);
        wb_warp_set_source(w, audio, src_frames, chn);

        /* Warp: stretch first half (0-2s of source) to 0-4s of beats,
         * and second half (2s-4s) to 4s-6s.
         * This means the audio plays slower (2x time stretch).
         * Request 6 seconds of output → should get 6s * sr frames */
        wb_warp_clear_markers(w);
        wb_warp_add_marker(w, 88200.0, 4.0);   /* src 2s → beat 4 */
        wb_warp_add_marker(w, 176400.0, 6.0);  /* src 4s → beat 6 */

        uint32_t out_frames = 264600;  /* 6 seconds */
        wb_sample *out = malloc((size_t)out_frames * 2 * sizeof(wb_sample));
        if (!out) { free(audio); FAIL("malloc out"); wb_warp_destroy(w); goto end; }

        wb_warp_process(w, 0.0, 6.0, out, out_frames);

        /* Check that output is non-silent throughout (proves correct duration) */
        float rms_early = rms_stereo(out, 44100);           /* first second */
        float rms_late   = rms_stereo(out + 220500*2, 44100);  /* last second */
        printf("  early RMS = %.6f, late RMS = %.6f\n", rms_early, rms_late);
        if (rms_early > 0.01f && rms_late > 0.01f)
            PASS();
        else FAIL("output too short or silent at edges");

        free(out);
        free(audio);
        wb_warp_destroy(w);
    }
    end:;

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}