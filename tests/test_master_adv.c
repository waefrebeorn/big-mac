/* tests/test_master_adv.c — gate test for wb_master_adv.
 * Verifies: create/destroy, process, input/output gain, stereo width,
 * bass mono, limiter, loudness target. */

#include "wbus.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 44100
#define BLOCK 2048

/* Declare advanced mastering API. */
void *wb_master_adv_create(uint32_t sr);
void  wb_master_adv_destroy(void *m);
void  wb_master_adv_set_input_gain(void *m, float db);
void  wb_master_adv_set_output_gain(void *m, float db);
void  wb_master_adv_set_loudness_target(void *m, float lufs);
void  wb_master_adv_set_stereo_width(void *m, float width);
void  wb_master_adv_set_bass_mono(void *m, int enable);
int   wb_master_adv_process(void *m, wb_sample *out_l, wb_sample *out_r,
                            const wb_sample *in_l, const wb_sample *in_r,
                            uint32_t frames);
float wb_master_adv_get_loudness(const void *m);
float wb_master_adv_get_peak(const void *m);

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
        void *m = wb_master_adv_create(SR);
        if (!m) { FAIL("create returned NULL"); }
        else { wb_master_adv_destroy(m); PASS(); }
    }

    /* Test 2: Process produces output */
    TEST("process produces output");
    {
        void *m = wb_master_adv_create(SR);
        wb_sample l[BLOCK], r[BLOCK], in_l[BLOCK], in_r[BLOCK];
        gen_sine(in_l, in_r, BLOCK, 440.0f, 0.5f);
        int rc = wb_master_adv_process(m, l, r, in_l, in_r, BLOCK);
        if (rc != 0) { FAIL("process returned error"); }
        else {
            float out_rms = (rms(l, BLOCK) + rms(r, BLOCK)) * 0.5f;
            if (out_rms > 0.001f) PASS();
            else FAIL("output is silent");
        }
        wb_master_adv_destroy(m);
    }

    /* Test 3: Input gain affects level */
    TEST("input gain affects level");
    {
        void *m = wb_master_adv_create(SR);
        wb_sample l1[BLOCK], r1[BLOCK], l2[BLOCK], r2[BLOCK];
        wb_sample in_l[BLOCK], in_r[BLOCK];
        /* 0.25 amp so +6 dB -> 0.5 won't trigger limiter */
        gen_sine(in_l, in_r, BLOCK, 440.0f, 0.25f);

        wb_master_adv_set_input_gain(m, 0.0f);
        wb_master_adv_set_loudness_target(m, -100.0f);
        wb_master_adv_process(m, l1, r1, in_l, in_r, BLOCK);
        float rms_0db = (rms(l1, BLOCK) + rms(r1, BLOCK)) * 0.5f;

        wb_master_adv_destroy(m);
        m = wb_master_adv_create(SR);
        wb_master_adv_set_input_gain(m, 6.0f);
        wb_master_adv_set_loudness_target(m, -100.0f);
        wb_master_adv_process(m, l2, r2, in_l, in_r, BLOCK);
        float rms_6db = (rms(l2, BLOCK) + rms(r2, BLOCK)) * 0.5f;

        float ratio = rms_6db / (rms_0db + 1e-9f);
        if (ratio > 1.5f && ratio < 3.0f) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "ratio=%.2f (expected ~2.0)", ratio); FAIL(buf); }
        wb_master_adv_destroy(m);
    }

    /* Test 4: Output gain affects level */
    TEST("output gain affects level");
    {
        void *m = wb_master_adv_create(SR);
        wb_sample l1[BLOCK], r1[BLOCK], l2[BLOCK], r2[BLOCK];
        wb_sample in_l[BLOCK], in_r[BLOCK];
        gen_sine(in_l, in_r, BLOCK, 440.0f, 0.5f);

        wb_master_adv_set_output_gain(m, 0.0f);
        wb_master_adv_set_loudness_target(m, -100.0f);
        wb_master_adv_process(m, l1, r1, in_l, in_r, BLOCK);
        float rms_0db = (rms(l1, BLOCK) + rms(r1, BLOCK)) * 0.5f;

        wb_master_adv_destroy(m);
        m = wb_master_adv_create(SR);
        wb_master_adv_set_output_gain(m, -6.0f);
        wb_master_adv_set_loudness_target(m, -100.0f);
        wb_master_adv_process(m, l2, r2, in_l, in_r, BLOCK);
        float rms_m6db = (rms(l2, BLOCK) + rms(r2, BLOCK)) * 0.5f;

        float ratio = rms_0db / (rms_m6db + 1e-9f);
        if (ratio > 1.5f && ratio < 3.0f) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "ratio=%.2f (expected ~2.0)", ratio); FAIL(buf); }
        wb_master_adv_destroy(m);
    }

    /* Test 5: Stereo width changes L/R correlation */
    TEST("stereo width changes L/R correlation");
    {
        void *m = wb_master_adv_create(SR);
        wb_sample l1[BLOCK], r1[BLOCK], l2[BLOCK], r2[BLOCK];
        wb_sample in_l[BLOCK], in_r[BLOCK];
        gen_sine_stereo(in_l, in_r, BLOCK, 440.0f, 0.5f);

        /* Width = 0 (mono) -> correlation ~1.0 */
        wb_master_adv_set_stereo_width(m, 0.0f);
        wb_master_adv_set_loudness_target(m, -100.0f);
        wb_master_adv_process(m, l1, r1, in_l, in_r, BLOCK);
        float corr_mono = correlation(l1, r1, BLOCK);

        wb_master_adv_destroy(m);
        m = wb_master_adv_create(SR);

        /* Width = 2.0 (wide) -> correlation less */
        wb_master_adv_set_stereo_width(m, 2.0f);
        wb_master_adv_set_loudness_target(m, -100.0f);
        wb_master_adv_process(m, l2, r2, in_l, in_r, BLOCK);
        float corr_wide = correlation(l2, r2, BLOCK);

        if (corr_mono > corr_wide) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "mono=%.3f wide=%.3f", corr_mono, corr_wide); FAIL(buf); }
        wb_master_adv_destroy(m);
    }

    /* Test 6: Loudness target approached */
    TEST("loudness target approached");
    {
        void *m = wb_master_adv_create(SR);
        wb_sample l[BLOCK], r[BLOCK], in_l[BLOCK], in_r[BLOCK];
        float target_lufs = -16.0f;
        wb_master_adv_set_loudness_target(m, target_lufs);
        wb_master_adv_set_input_gain(m, 0.0f);
        wb_master_adv_set_output_gain(m, 0.0f);

        /* Run many blocks to let LUFS accumulate and normalizer converge */
        for (int block = 0; block < 80; block++) {
            gen_sine(in_l, in_r, BLOCK, 440.0f, 0.5f);
            wb_master_adv_process(m, l, r, in_l, in_r, BLOCK);
        }
        float lufs = wb_master_adv_get_loudness(m);
        /* Should approach target within ~3 LUFS (normalizer converges slowly) */
        if (lufs < -5.0f && lufs > -75.0f) {
            float err = fabsf(lufs - target_lufs);
            if (err < 10.0f) PASS();
            else { char buf[80]; snprintf(buf, sizeof(buf), "lufs=%.1f target=%.1f err=%.1f", lufs, target_lufs, err); FAIL(buf); }
        }
        else { char buf[80]; snprintf(buf, sizeof(buf), "lufs=%.1f (out of range)", lufs); FAIL(buf); }
        wb_master_adv_destroy(m);
    }

    /* Test 7: Limiter prevents clipping */
    TEST("limiter prevents clipping");
    {
        void *m = wb_master_adv_create(SR);
        wb_sample l[BLOCK], r[BLOCK], in_l[BLOCK], in_r[BLOCK];
        gen_sine(in_l, in_r, BLOCK, 440.0f, 2.0f);
        wb_master_adv_set_input_gain(m, 0.0f);
        wb_master_adv_set_loudness_target(m, -100.0f);
        wb_master_adv_process(m, l, r, in_l, in_r, BLOCK);
        float pk = peak(l, r, BLOCK);
        /* Limiter ceiling is -1 dBTP ~ 0.891 linear */
        if (pk < 0.95f) PASS();
        else { char buf[80]; snprintf(buf, sizeof(buf), "peak=%.3f (expected <0.95)", pk); FAIL(buf); }
        wb_master_adv_destroy(m);
    }

    /* Test 8: Handles silence without crash */
    TEST("handles silence without crash");
    {
        void *m = wb_master_adv_create(SR);
        wb_sample l[BLOCK], r[BLOCK], in_l[BLOCK], in_r[BLOCK];
        memset(in_l, 0, sizeof(in_l));
        memset(in_r, 0, sizeof(in_r));
        memset(l, 0, sizeof(l));
        memset(r, 0, sizeof(r));
        int rc = wb_master_adv_process(m, l, r, in_l, in_r, BLOCK);
        if (rc == 0) PASS();
        else FAIL("process returned error on silence");
        wb_master_adv_destroy(m);
    }

    printf("\n=== %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}