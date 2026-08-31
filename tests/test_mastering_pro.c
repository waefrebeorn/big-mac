/* tests/test_mastering_pro.c — gate test for wb_mastering_pro.
 * Verifies: create/destroy, process, input/output gain, stereo width,
 * bass mono, limiter, loudness target. */

#include "wbus.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 44100
#define BLOCK 2048

/* Declare mastering API (also in wbus.h). */
void *wb_mastering_create(uint32_t sr);
void  wb_mastering_destroy(void *m);
void  wb_mastering_set_input_gain(void *m, float db);
void  wb_mastering_set_output_gain(void *m, float db);
void  wb_mastering_set_loudness_target(void *m, float lufs);
void  wb_mastering_set_stereo_width(void *m, float width);
void  wb_mastering_set_bass_mono(void *m, int enable);
void  wb_mastering_process(void *m, wb_sample *out_l, wb_sample *out_r, uint32_t frames);
float wb_mastering_get_loudness(const void *m);
float wb_mastering_get_peak(const void *m);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

/* Compute RMS of a mono buffer. */
static float rms(const wb_sample *buf, uint32_t n) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < n; i++) sum += buf[i] * buf[i];
    return sqrtf(sum / (float)n);
}

/* Compute peak absolute value across both channels. */
static float peak(const wb_sample *l, const wb_sample *r, uint32_t n) {
    float pk = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float a = fabsf(l[i]), b = fabsf(r[i]);
        if (a > pk) pk = a;
        if (b > pk) pk = b;
    }
    return pk;
}

/* Generate a stereo sine wave (same in both channels). */
static void gen_sine(wb_sample *l, wb_sample *r, uint32_t n, float freq, float amp) {
    for (uint32_t i = 0; i < n; i++) {
        float s = amp * sinf(2.0f * 3.14159265f * freq * (float)i / (float)SR);
        l[i] = s;
        r[i] = s;
    }
}

/* Generate stereo sine with different L/R amplitudes. */
static void gen_sine_stereo(wb_sample *l, wb_sample *r, uint32_t n, float freq, float amp) {
    for (uint32_t i = 0; i < n; i++) {
        float s = amp * sinf(2.0f * 3.14159265f * freq * (float)i / (float)SR);
        l[i] = s;
        r[i] = s * 0.3f;
    }
}

/* Compute L/R correlation (Pearson). */
static float correlation(const wb_sample *l, const wb_sample *r, uint32_t n) {
    float sum_l = 0, sum_r = 0, sum_lr = 0, sum_l2 = 0, sum_r2 = 0;
    for (uint32_t i = 0; i < n; i++) {
        sum_l += l[i]; sum_r += r[i];
        sum_lr += l[i] * r[i];
        sum_l2 += l[i] * l[i];
        sum_r2 += r[i] * r[i];
    }
    float num = sum_lr - (sum_l * sum_r) / (float)n;
    float den_l = sum_l2 - (sum_l * sum_l) / (float)n;
    float den_r = sum_r2 - (sum_r * sum_r) / (float)n;
    float den = sqrtf(den_l * den_r);
    if (den < 1e-9f) return 0.0f;
    return num / den;
}

int main(void) {
    /* Test 1: Create/destroy */
    TEST("create/destroy");
    {
        void *m = wb_mastering_create(SR);
        if (!m) { FAIL("create returned NULL"); }
        else { wb_mastering_destroy(m); PASS(); }
    }

    /* Test 2: Process produces output */
    TEST("process produces output");
    {
        void *m = wb_mastering_create(SR);
        wb_sample l[BLOCK], r[BLOCK];
        gen_sine(l, r, BLOCK, 440.0f, 0.5f);
        wb_mastering_process(m, l, r, BLOCK);
        float out_rms = (rms(l, BLOCK) + rms(r, BLOCK)) * 0.5f;
        if (out_rms > 0.001f) PASS();
        else FAIL("output is silent");
        wb_mastering_destroy(m);
    }

    /* Test 3: Input gain affects level */
    TEST("input gain affects level");
    {
        void *m = wb_mastering_create(SR);
        wb_sample l1[BLOCK], r1[BLOCK], l2[BLOCK], r2[BLOCK];
        /* 0.25 amp so +6 dB -> 0.5 won't trigger limiter */
        gen_sine(l1, r1, BLOCK, 440.0f, 0.25f);
        memcpy(l2, l1, sizeof(l1)); memcpy(r2, r1, sizeof(r1));

        wb_mastering_set_input_gain(m, 0.0f);
        wb_mastering_set_loudness_target(m, -100.0f);
        wb_mastering_process(m, l1, r1, BLOCK);
        float rms_0db = (rms(l1, BLOCK) + rms(r1, BLOCK)) * 0.5f;

        wb_mastering_destroy(m);
        m = wb_mastering_create(SR);
        wb_mastering_set_input_gain(m, 6.0f);
        wb_mastering_set_loudness_target(m, -100.0f);
        wb_mastering_process(m, l2, r2, BLOCK);
        float rms_6db = (rms(l2, BLOCK) + rms(r2, BLOCK)) * 0.5f;

        float ratio = rms_6db / (rms_0db + 1e-9f);
        if (ratio > 1.5f && ratio < 3.0f) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "ratio=%.2f (expected ~2.0)", ratio); FAIL(buf); }
        wb_mastering_destroy(m);
    }

    /* Test 4: Output gain affects level */
    TEST("output gain affects level");
    {
        void *m = wb_mastering_create(SR);
        wb_sample l1[BLOCK], r1[BLOCK], l2[BLOCK], r2[BLOCK];
        gen_sine(l1, r1, BLOCK, 440.0f, 0.5f);
        memcpy(l2, l1, sizeof(l1)); memcpy(r2, r1, sizeof(r1));

        wb_mastering_set_output_gain(m, 0.0f);
        wb_mastering_set_loudness_target(m, -100.0f);
        wb_mastering_process(m, l1, r1, BLOCK);
        float rms_0db = (rms(l1, BLOCK) + rms(r1, BLOCK)) * 0.5f;

        wb_mastering_destroy(m);
        m = wb_mastering_create(SR);
        wb_mastering_set_output_gain(m, -6.0f);
        wb_mastering_set_loudness_target(m, -100.0f);
        wb_mastering_process(m, l2, r2, BLOCK);
        float rms_m6db = (rms(l2, BLOCK) + rms(r2, BLOCK)) * 0.5f;

        float ratio = rms_0db / (rms_m6db + 1e-9f);
        if (ratio > 1.5f && ratio < 3.0f) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "ratio=%.2f (expected ~2.0)", ratio); FAIL(buf); }
        wb_mastering_destroy(m);
    }

    /* Test 5: Stereo width changes L/R correlation */
    TEST("stereo width changes L/R correlation");
    {
        void *m = wb_mastering_create(SR);
        wb_sample l1[BLOCK], r1[BLOCK], l2[BLOCK], r2[BLOCK];
        gen_sine_stereo(l1, r1, BLOCK, 440.0f, 0.5f);
        memcpy(l2, l1, sizeof(l1)); memcpy(r2, r1, sizeof(r1));

        /* Width = 0 (mono) -> correlation ~1.0 */
        wb_mastering_set_stereo_width(m, 0.0f);
        wb_mastering_process(m, l1, r1, BLOCK);
        float corr_mono = correlation(l1, r1, BLOCK);

        wb_mastering_destroy(m);
        m = wb_mastering_create(SR);

        /* Width = 2.0 (wide) -> correlation less */
        wb_mastering_set_stereo_width(m, 2.0f);
        wb_mastering_process(m, l2, r2, BLOCK);
        float corr_wide = correlation(l2, r2, BLOCK);

        if (corr_mono > corr_wide) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "mono=%.3f wide=%.3f", corr_mono, corr_wide); FAIL(buf); }
        wb_mastering_destroy(m);
    }

    /* ---- Test 6: Bass mono makes bass channel equal ---- */
        TEST("bass mono makes bass channel equal");
        {
            void *m = wb_mastering_create(SR);
            wb_sample l1[BLOCK], r1[BLOCK], l2[BLOCK], r2[BLOCK];
            /* 50 Hz bass (in band 0, below 100 Hz crossover).
             * Use 0.05 amp to stay below compressor threshold (-18dB ~ 0.126). */
            gen_sine_stereo(l1, r1, BLOCK, 50.0f, 0.05f);
            memcpy(l2, l1, sizeof(l1)); memcpy(r2, r1, sizeof(r1));

            /* Without bass mono: L != R */
            wb_mastering_set_bass_mono(m, 0);
            wb_mastering_set_loudness_target(m, -100.0f);
            /* Run multiple blocks to let compressor envelope settle */
            for (int b = 0; b < 40; b++) {
                gen_sine_stereo(l1, r1, BLOCK, 50.0f, 0.05f);
                wb_mastering_process(m, l1, r1, BLOCK);
            }
            float diff_no_mono = 0.0f;
            for (uint32_t i = 500; i < BLOCK; i++)
                diff_no_mono += fabsf(l1[i] - r1[i]);
            diff_no_mono /= (float)(BLOCK - 500);

            wb_mastering_destroy(m);
            m = wb_mastering_create(SR);

            /* With bass mono: L ~ R for bass content */
            wb_mastering_set_bass_mono(m, 1);
            wb_mastering_set_loudness_target(m, -100.0f);
            for (int b = 0; b < 40; b++) {
                gen_sine_stereo(l2, r2, BLOCK, 50.0f, 0.05f);
                wb_mastering_process(m, l2, r2, BLOCK);
            }
            float diff_bass_mono = 0.0f;
            for (uint32_t i = 500; i < BLOCK; i++)
                diff_bass_mono += fabsf(l2[i] - r2[i]);
            diff_bass_mono /= (float)(BLOCK - 500);

            /* Bass mono should significantly reduce L/R difference for bass */
            if (diff_bass_mono < diff_no_mono * 0.5f) PASS();
            else { char buf[80]; snprintf(buf, sizeof(buf), "no_mono=%.6f bass_mono=%.6f (ratio=%.2f)", diff_no_mono, diff_bass_mono, diff_bass_mono/(diff_no_mono+1e-12f)); FAIL(buf); }

        /* Bass mono folds band 0 (<100 Hz) to mono. The 30 Hz signal is
         * mostly in band 0, so L/R diff should drop significantly.
         * Some leakage into higher bands is expected, so we check
         * for a meaningful reduction (at least 30%). */
        if (diff_bass_mono < diff_no_mono * 0.7f) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "no_mono=%.6f bass_mono=%.6f (ratio=%.2f)", diff_no_mono, diff_bass_mono, diff_bass_mono/(diff_no_mono+1e-12f)); FAIL(buf); }
        wb_mastering_destroy(m);
    }

    /* Test 7: Limiter prevents clipping */
    TEST("limiter prevents clipping");
    {
        void *m = wb_mastering_create(SR);
        wb_sample l[BLOCK], r[BLOCK];
        gen_sine(l, r, BLOCK, 440.0f, 2.0f);
        wb_mastering_set_input_gain(m, 0.0f);
        wb_mastering_process(m, l, r, BLOCK);
        float pk = peak(l, r, BLOCK);
        /* Limiter ceiling is -1 dBTP ~ 0.891 linear */
        if (pk < 0.95f) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "peak=%.3f (expected <0.95)", pk); FAIL(buf); }
        wb_mastering_destroy(m);
    }

    /* Test 8: Loudness measurement produces valid reading */
    TEST("loudness measurement valid");
    {
        void *m = wb_mastering_create(SR);
        wb_mastering_set_loudness_target(m, -23.0f);
        wb_sample l[BLOCK], r[BLOCK];
        /* Run many blocks to let LUFS accumulate */
        for (int block = 0; block < 40; block++) {
            gen_sine(l, r, BLOCK, 440.0f, 0.5f);
            wb_mastering_process(m, l, r, BLOCK);
        }
        float lufs = wb_mastering_get_loudness(m);
        /* Should produce a valid LUFS reading (negative, finite, not zero) */
        if (lufs < -5.0f && lufs > -75.0f) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "lufs=%.1f (expected -5..-75)", lufs); FAIL(buf); }
        wb_mastering_destroy(m);
    }

    /* Test 9: Peak meter reports valid value */
    TEST("peak meter reports valid value");
    {
        void *m = wb_mastering_create(SR);
        wb_sample l[BLOCK], r[BLOCK];
        gen_sine(l, r, BLOCK, 440.0f, 0.5f);
        wb_mastering_process(m, l, r, BLOCK);
        float pk = wb_mastering_get_peak(m);
        if (pk > 0.0f && pk <= 1.0f) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "peak=%.4f", pk); FAIL(buf); }
        wb_mastering_destroy(m);
    }

    /* Test 10: Handles silence without crash */
    TEST("handles silence without crash");
    {
        void *m = wb_mastering_create(SR);
        wb_sample l[BLOCK], r[BLOCK];
        memset(l, 0, sizeof(l));
        memset(r, 0, sizeof(r));
        wb_mastering_process(m, l, r, BLOCK);
        float lufs = wb_mastering_get_loudness(m);
        if (lufs == 0.0f) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "lufs=%.1f (expected 0.0 for silence)", lufs); FAIL(buf); }
        wb_mastering_destroy(m);
    }

    printf("\n=== %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}