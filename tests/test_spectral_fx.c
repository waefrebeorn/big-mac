/* tests/test_spectral_fx.c — spectral effects gate test.
 * Pure C11, standalone: only needs wb_spectral_fx.o + wb_fft.o. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_spectral_fx.h"

#define SR 44100

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

/* Compute RMS of a buffer */
static float compute_rms(const wb_sample *buf, uint32_t n) {
    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++) sum += (double)buf[i] * (double)buf[i];
    return (float)sqrt(sum / (double)n);
}

/* Check for NaN/Inf */
static int check_finite(const wb_sample *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (buf[i] != buf[i]) return 0;       /* NaN */
        if (buf[i] > 1e18f || buf[i] < -1e18f) return 0; /* Inf */
    }
    return 1;
}

/* Goertzel-like energy at a specific frequency */
static float band_energy(const wb_sample *buf, uint32_t n, float freq, float sr) {
    double s = 0.0, c = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double phase = 2.0 * M_PI * freq * (double)i / (double)sr;
        s += (double)buf[i] * sin(phase);
        c += (double)buf[i] * cos(phase);
    }
    return (float)sqrt(s * s + c * c) / (float)n;
}

int main(void) {
    printf("=== test_spectral_fx ===\n\n");

    /* Test 1: Create / Destroy */
    TEST("create/destroy");
    {
        void *sf = wb_spectral_fx_create(SR);
        if (sf) {
            wb_spectral_fx_destroy(sf);
            PASS();
        } else {
            FAIL("create returned NULL");
        }
    }

    /* Test 2: Process produces output */
    TEST("process produces output");
    {
        void *sf = wb_spectral_fx_create(SR);
        if (!sf) { FAIL("create failed"); goto test3; }

        uint32_t frames = 4096;
        wb_sample *in = (wb_sample *)calloc(frames, sizeof(wb_sample));
        wb_sample *out = (wb_sample *)calloc(frames, sizeof(wb_sample));
        if (!in || !out) { FAIL("alloc"); free(in); free(out); wb_spectral_fx_destroy(sf); goto test3; }

        /* 440 Hz sine input */
        for (uint32_t i = 0; i < frames; i++)
            in[i] = (wb_sample)sin(2.0 * M_PI * 440.0 * (double)i / (double)SR);

        wb_spectral_fx_set_type(sf, WB_SPECTRAL_RESONATOR);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_MIX, 0.5f);
        wb_spectral_fx_process(sf, out, in, frames);

        float rms = compute_rms(out, frames);
        printf("  output RMS = %.6f\n", rms);
        if (rms > 0.001f) PASS(); else FAIL("output is silent");

        free(in); free(out);
        wb_spectral_fx_destroy(sf);
    }
test3:

    /* Test 3: Resonator changes spectrum (boosts harmonics) */
    TEST("resonator changes spectrum");
    {
        void *sf = wb_spectral_fx_create(SR);
        if (!sf) { FAIL("create failed"); goto test4; }

        uint32_t frames = 8192;
        wb_sample *in = (wb_sample *)calloc(frames, sizeof(wb_sample));
        wb_sample *out = (wb_sample *)calloc(frames, sizeof(wb_sample));
        if (!in || !out) { FAIL("alloc"); free(in); free(out); wb_spectral_fx_destroy(sf); goto test4; }

        /* 220 Hz sine — resonator at 220 Hz should boost 220, 440, 660, ... */
        for (uint32_t i = 0; i < frames; i++)
            in[i] = (wb_sample)sin(2.0 * M_PI * 220.0 * (double)i / (double)SR);

        wb_spectral_fx_set_type(sf, WB_SPECTRAL_RESONATOR);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_FREQUENCY, 220.0f);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_AMOUNT, 0.8f);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_DECAY, 0.7f);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_MIX, 1.0f); /* full wet */
        wb_spectral_fx_process(sf, out, in, frames);

        /* Measure energy at 220 Hz (fundamental) and 440 Hz (2nd harmonic) */
        float e220 = band_energy(out, frames, 220.0f, (float)SR);
        float e440 = band_energy(out, frames, 440.0f, (float)SR);
        printf("  energy @ 220 Hz = %.6f, @ 440 Hz = %.6f\n", e220, e440);

        /* The resonator should boost the fundamental significantly */
        float in_e220 = band_energy(in, frames, 220.0f, (float)SR);
        printf("  input energy @ 220 Hz = %.6f\n", in_e220);
        if (e220 > in_e220 * 1.2f) PASS();
        else FAIL("resonator did not boost fundamental");

        free(in); free(out);
        wb_spectral_fx_destroy(sf);
    }
test4:

    /* Test 4: Blur changes spectrum (smears energy) */
    TEST("blur changes spectrum");
    {
        void *sf = wb_spectral_fx_create(SR);
        if (!sf) { FAIL("create failed"); goto test5; }

        uint32_t frames = 8192;
        wb_sample *in = (wb_sample *)calloc(frames, sizeof(wb_sample));
        wb_sample *out = (wb_sample *)calloc(frames, sizeof(wb_sample));
        if (!in || !out) { FAIL("alloc"); free(in); free(out); wb_spectral_fx_destroy(sf); goto test5; }

        /* Chirp signal: sweeps 200-2000 Hz */
        for (uint32_t i = 0; i < frames; i++) {
            double t = (double)i / (double)SR;
            double phase = 2.0 * M_PI * (200.0 * t + (1800.0 / (2.0 * 2.0)) * t * t);
            in[i] = (wb_sample)sin(phase);
        }

        wb_spectral_fx_set_type(sf, WB_SPECTRAL_BLUR);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_AMOUNT, 0.7f);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_MIX, 1.0f);
        wb_spectral_fx_process(sf, out, in, frames);

        /* Blur should produce output with different spectral content */
        float rms_in = compute_rms(in, frames);
        float rms_out = compute_rms(out, frames);
        printf("  input RMS = %.6f, output RMS = %.6f\n", rms_in, rms_out);

        /* Output should be non-trivially different from input */
        /* Compare energy at a mid frequency */
        float e_in = band_energy(in, frames, 800.0f, (float)SR);
        float e_out = band_energy(out, frames, 800.0f, (float)SR);
        printf("  energy @ 800 Hz: in=%.6f out=%.6f\n", e_in, e_out);

        if (rms_out > 0.001f && fabsf(e_out - e_in) > 0.0001f) PASS();
        else FAIL("blur did not change spectrum");

        free(in); free(out);
        wb_spectral_fx_destroy(sf);
    }
test5:

    /* Test 5: Time effect produces output */
    TEST("time effect produces output");
    {
        void *sf = wb_spectral_fx_create(SR);
        if (!sf) { FAIL("create failed"); goto test6; }

        uint32_t frames = 8192;
        wb_sample *in = (wb_sample *)calloc(frames, sizeof(wb_sample));
        wb_sample *out = (wb_sample *)calloc(frames, sizeof(wb_sample));
        if (!in || !out) { FAIL("alloc"); free(in); free(out); wb_spectral_fx_destroy(sf); goto test6; }

        /* Complex input: sum of sines */
        for (uint32_t i = 0; i < frames; i++) {
            double s = 0.0;
            s += 0.5 * sin(2.0 * M_PI * 330.0 * (double)i / (double)SR);
            s += 0.3 * sin(2.0 * M_PI * 660.0 * (double)i / (double)SR);
            s += 0.2 * sin(2.0 * M_PI * 990.0 * (double)i / (double)SR);
            in[i] = (wb_sample)s;
        }

        wb_spectral_fx_set_type(sf, WB_SPECTRAL_TIME);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_AMOUNT, 0.8f); /* trigger freeze */
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_DECAY, 0.7f);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_MIX, 0.7f);
        wb_spectral_fx_process(sf, out, in, frames);

        float rms = compute_rms(out, frames);
        printf("  output RMS = %.6f\n", rms);
        if (rms > 0.001f) PASS(); else FAIL("time effect produced no output");

        free(in); free(out);
        wb_spectral_fx_destroy(sf);
    }
test6:

    /* Test 6: Output finite (no NaN/Inf) */
    TEST("output finite (no NaN/Inf)");
    {
        void *sf = wb_spectral_fx_create(SR);
        if (!sf) { FAIL("create failed"); goto test7; }

        uint32_t frames = 16384;
        wb_sample *in = (wb_sample *)calloc(frames, sizeof(wb_sample));
        wb_sample *out = (wb_sample *)calloc(frames, sizeof(wb_sample));
        if (!in || !out) { FAIL("alloc"); free(in); free(out); wb_spectral_fx_destroy(sf); goto test7; }

        /* Test with various inputs */
        /* a) loud signal */
        for (uint32_t i = 0; i < frames; i++)
            in[i] = 0.9f * (wb_sample)sin(2.0 * M_PI * 440.0 * (double)i / (double)SR);

        int all_finite = 1;

        /* Test resonator with extreme params */
        wb_spectral_fx_set_type(sf, WB_SPECTRAL_RESONATOR);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_AMOUNT, 1.0f);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_FREQUENCY, 100.0f);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_DECAY, 0.99f);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_MIX, 1.0f);
        wb_spectral_fx_process(sf, out, in, frames);
        if (!check_finite(out, frames)) { all_finite = 0; printf("  resonator: non-finite\n"); }

        /* Test blur with max amount */
        wb_spectral_fx_set_type(sf, WB_SPECTRAL_BLUR);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_AMOUNT, 1.0f);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_MIX, 1.0f);
        wb_spectral_fx_process(sf, out, in, frames);
        if (!check_finite(out, frames)) { all_finite = 0; printf("  blur: non-finite\n"); }

        /* Test time with max params */
        wb_spectral_fx_set_type(sf, WB_SPECTRAL_TIME);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_AMOUNT, 1.0f);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_DECAY, 0.99f);
        wb_spectral_fx_set_param(sf, WB_SPECTRAL_PARAM_MIX, 1.0f);
        wb_spectral_fx_process(sf, out, in, frames);
        if (!check_finite(out, frames)) { all_finite = 0; printf("  time: non-finite\n"); }

        /* Test with silence */
        memset(in, 0, frames * sizeof(wb_sample));
        wb_spectral_fx_set_type(sf, WB_SPECTRAL_RESONATOR);
        wb_spectral_fx_process(sf, out, in, frames);
        if (!check_finite(out, frames)) { all_finite = 0; printf("  silence: non-finite\n"); }

        if (all_finite) PASS(); else FAIL("non-finite values detected");

        free(in); free(out);
        wb_spectral_fx_destroy(sf);
    }
test7:

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}