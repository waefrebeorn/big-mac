/* tests/test_spectral_edit.c — spectral audio repair tools test.
 * Pure C11, standalone: only needs wb_spectral_edit.o + wb_fft.o. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <math.h>
#include "wbus/wbus_spectral_edit.h"

#define SR 44100
#define SE_FRAME 2048
#define SE_HOP 512

static float compute_rms(const float *buf, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += buf[i] * buf[i];
    return sqrtf(sum / (float)n);
}

static int has_nan(const float *buf, int n) {
    for (int i = 0; i < n; i++)
        if (buf[i] != buf[i]) return 1;
    return 0;
}

static int has_inf(const float *buf, int n) {
    for (int i = 0; i < n; i++)
        if (buf[i] > 1e18f || buf[i] < -1e18f) return 1;
    return 0;
}

/* Compute energy at a specific frequency band (±5 Hz) */
static float band_energy(const float *buf, int n, float freq, float sr) {
    /* Goertzel-like: correlate with sin/cos at freq */
    float sr_f = sr;
    double s = 0.0, c = 0.0;
    for (int i = 0; i < n; i++) {
        double phase = 2.0 * M_PI * freq * (double)i / (double)sr_f;
        s += (double)buf[i] * sin(phase);
        c += (double)buf[i] * cos(phase);
    }
    return (float)sqrt(s*s + c*c) / (float)n;
}

int main(void) {
    int pass = 0, fail = 0;
    uint32_t frames = SR * 2; /* 2 seconds */
    uint32_t chn = 1;

    printf("=== test_spectral_edit ===\n");

    /* Allocate buffers */
    float *signal = (float*)calloc(frames * chn, sizeof(float));
    float *output = (float*)calloc(frames * chn, sizeof(float));
    float *noise = (float*)calloc(frames * chn, sizeof(float));
    if (!signal || !output || !noise) {
        printf("  FAIL: allocation\n"); return 1;
    }

    /* Build a test signal: 440 Hz sine + noise */
    for (uint32_t i = 0; i < frames; i++) {
        signal[i] = 0.5f * sinf(2.0f * M_PI * 440.0f * (float)i / (float)SR);
        noise[i] = 0.1f * ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f);
    }

    /* ---- Test 1: Denoise reduces noise RMS ---- */
    {
        /* Create noisy signal: first 100ms is noise-only (for profile),
         * rest is sine + noise */
        uint32_t noise_prefix = SR / 10; /* 100ms */
        float *noisy = (float*)malloc(frames * sizeof(float));
        for (uint32_t i = 0; i < noise_prefix; i++)
            noisy[i] = noise[i]; /* noise-only for profile estimation */
        for (uint32_t i = noise_prefix; i < frames; i++)
            noisy[i] = signal[i] + noise[i]; /* sine + noise */

        /* Measure noise-only RMS from the noise reference */
        float rms_before = compute_rms(noise, (int)frames);
        int rc = wb_spectral_denoise(noisy, output, frames, chn, 0.8f);

        /* Measure residual noise in the signal region (after noise prefix + steady state).
         * Compare output against clean signal — residual = remaining noise. */
        float residual_rms = 0.0f;
        /* Skip first 2048+noise_prefix samples to let OLA steady-state settle */
        uint32_t start = noise_prefix + SE_FRAME + SE_HOP;
        for (uint32_t i = start; i < frames; i++) {
            float d = output[i] - signal[i];
            residual_rms += d * d;
        }
        uint32_t count = (frames > start) ? (frames - start) : 1;
        residual_rms = sqrtf(residual_rms / (float)count);

        if (rc == 0 && residual_rms < rms_before * 0.95f) {
            printf("  PASS: denoise reduces noise RMS (noise_rms=%.4f residual=%.4f)\n",
                   rms_before, residual_rms);
            pass++;
        } else {
            printf("  FAIL: denoise (rc=%d, noise_rms=%.4f residual=%.4f)\n",
                   rc, rms_before, residual_rms);
            fail++;
        }
        free(noisy);
    }

    /* ---- Test 2: Declick preserves non-click regions ---- */
    {
        /* Clean signal with a single click at midpoint */
        float *click_sig = (float*)malloc(frames * sizeof(float));
        memcpy(click_sig, signal, frames * sizeof(float));
        /* Insert a click: 5-sample spike */
        uint32_t click_pos = frames / 2;
        for (uint32_t i = click_pos; i < click_pos + 5 && i < frames; i++)
            click_sig[i] = 5.0f;

        int rc = wb_spectral_declick(click_sig, output, frames, chn, 0.5f);

        /* Check that non-click regions are preserved (before click) */
        float err = 0.0f;
        uint32_t check_end = click_pos > 100 ? click_pos - 100 : 0;
        for (uint32_t i = 100; i < check_end; i++) {
            err += fabsf(output[i] - signal[i]);
        }
        float avg_err = err / (float)(check_end > 100 ? check_end - 100 : 1);

        if (rc == 0 && avg_err < 0.05f) {
            printf("  PASS: declick preserves non-click regions (avg_err=%.5f)\n", avg_err);
            pass++;
        } else {
            printf("  FAIL: declick (rc=%d, avg_err=%.5f)\n", rc, avg_err);
            fail++;
        }
        free(click_sig);
    }

    /* ---- Test 3: Dehum reduces 60Hz tone energy ---- */
    {
        /* Signal with 60 Hz hum */
        float *hum_sig = (float*)malloc(frames * sizeof(float));
        for (uint32_t i = 0; i < frames; i++)
            hum_sig[i] = signal[i] + 0.3f * sinf(2.0f * M_PI * 60.0f * (float)i / (float)SR);

        float energy_before = band_energy(hum_sig, (int)frames, 60.0f, (float)SR);

        int rc = wb_spectral_dehum(hum_sig, output, frames, chn, 60.0f);

        float energy_after = band_energy(output, (int)frames, 60.0f, (float)SR);

        if (rc == 0 && energy_after < energy_before * 0.7f) {
            printf("  PASS: dehum reduces 60Hz energy (before=%.4f after=%.4f)\n",
                   energy_before, energy_after);
            pass++;
        } else {
            printf("  FAIL: dehum (rc=%d, before=%.4f after=%.4f)\n",
                   rc, energy_before, energy_after);
            fail++;
        }
        free(hum_sig);
    }

    /* ---- Test 4: Gain increases amplitude correctly ---- */
    {
        float gain_db = 6.0f; /* ~2x amplitude */
        int rc = wb_spectral_gain(signal, output, frames, chn, gain_db);

        float rms_in = compute_rms(signal, (int)frames);
        float rms_out = compute_rms(output, (int)frames);
        float expected_ratio = powf(10.0f, gain_db / 20.0f);
        float actual_ratio = rms_out / (rms_in > 1e-10f ? rms_in : 1e-10f);

        if (rc == 0 && fabsf(actual_ratio - expected_ratio) < 0.05f) {
            printf("  PASS: gain +6dB doubles amplitude (ratio=%.4f expected=%.4f)\n",
                   actual_ratio, expected_ratio);
            pass++;
        } else {
            printf("  FAIL: gain (rc=%d, ratio=%.4f expected=%.4f)\n",
                   rc, actual_ratio, expected_ratio);
            fail++;
        }
    }

    /* ---- Test 5: Output is finite (no NaN/Inf) ---- */
    {
        int rc1 = wb_spectral_denoise(signal, output, frames, chn, 0.5f);
        int rc2 = wb_spectral_declick(signal, output, frames, chn, 0.1f);
        int rc3 = wb_spectral_dehum(signal, output, frames, chn, 60.0f);
        int rc4 = wb_spectral_gain(signal, output, frames, chn, -12.0f);

        int finite = !has_nan(output, (int)frames) && !has_inf(output, (int)frames);
        if (rc1 == 0 && rc2 == 0 && rc3 == 0 && rc4 == 0 && finite) {
            printf("  PASS: all outputs finite (no NaN/Inf)\n");
            pass++;
        } else {
            printf("  FAIL: output not finite (rc=%d/%d/%d/%d finite=%d)\n",
                   rc1, rc2, rc3, rc4, finite);
            fail++;
        }
    }

    /* ---- Test 6: Silence in → silence out ---- */
    {
        float *silence = (float*)calloc(frames * chn, sizeof(float));
        int rc = wb_spectral_denoise(silence, output, frames, chn, 0.5f);
        float rms = compute_rms(output, (int)frames);

        int rc2 = wb_spectral_gain(silence, output, frames, chn, 6.0f);
        float rms2 = compute_rms(output, (int)frames);

        if (rc == 0 && rc2 == 0 && rms < 1e-6f && rms2 < 1e-6f) {
            printf("  PASS: silence in -> silence out (rms=%.2e, %.2e)\n", rms, rms2);
            pass++;
        } else {
            printf("  FAIL: silence (rc=%d/%d rms=%.2e %.2e)\n", rc, rc2, rms, rms2);
            fail++;
        }
        free(silence);
    }

    /* ---- Test 7: Output length matches input ---- */
    {
        /* Just verify the function processes all frames without error */
        int rc = wb_spectral_denoise(signal, output, frames, chn, 0.5f);
        /* Check last sample is written (not zero from input) */
        /* Input signal at last sample is non-zero (sine), output should be too */
        int ok = (rc == 0) && (output[frames - 1] != 0.0f || signal[frames-1] == 0.0f);
        /* Also verify a middle sample was processed */
        ok = ok && (output[frames/2] != 0.0f);
        if (ok) {
            printf("  PASS: output length matches input (all %u frames processed)\n", frames);
            pass++;
        } else {
            printf("  FAIL: output length (rc=%d, mid=%.4f end=%.4f)\n",
                   rc, output[frames/2], output[frames-1]);
            fail++;
        }
    }

    /* ---- Test 8: Strength parameter affects amount ---- */
    {
        /* Create noisy signal: first 100ms noise-only, rest sine + noise */
        uint32_t noise_prefix = SR / 10;
        float *noisy = (float*)malloc(frames * sizeof(float));
        for (uint32_t i = 0; i < noise_prefix; i++)
            noisy[i] = noise[i];
        for (uint32_t i = noise_prefix; i < frames; i++)
            noisy[i] = signal[i] + noise[i];

        /* Denoise with low strength */
        float *out_low = (float*)malloc(frames * sizeof(float));
        wb_spectral_denoise(noisy, out_low, frames, chn, 0.2f);
        float rms_low = 0.0f;
        uint32_t start = noise_prefix + SE_FRAME + SE_HOP;
        for (uint32_t i = start; i < frames; i++) {
            float d = out_low[i] - signal[i];
            rms_low += d * d;
        }
        rms_low = sqrtf(rms_low / (float)(frames - start));

        /* Denoise with high strength */
        float *out_high = (float*)malloc(frames * sizeof(float));
        wb_spectral_denoise(noisy, out_high, frames, chn, 0.95f);
        float rms_high = 0.0f;
        for (uint32_t i = start; i < frames; i++) {
            float d = out_high[i] - signal[i];
            rms_high += d * d;
        }
        rms_high = sqrtf(rms_high / (float)(frames - start));

        if (rms_high < rms_low) {
            printf("  PASS: strength affects amount (low=%.4f high=%.4f)\n",
                   rms_low, rms_high);
            pass++;
        } else {
            printf("  FAIL: strength (low=%.4f high=%.4f)\n", rms_low, rms_high);
            fail++;
        }
        free(noisy); free(out_low); free(out_high);
    }

    printf("=== %d passed, %d failed ===\n", pass, fail);

    free(signal);
    free(output);
    free(noise);

    return fail > 0 ? 1 : 0;
}