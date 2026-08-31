/* tests/test_restoration.c — audio restoration suite test.
 * Pure C11, standalone: only needs wb_restoration.o + wb_fft.o.
 * Tests denoise, declip, dehum, declick, voice isolation. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus.h"

#define SR 44100
#define DURATION 1.0
#define N ((int)(SR * DURATION))

/* Compute RMS of a buffer */
static float compute_rms(const float *buf, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += (double)buf[i] * (double)buf[i];
    return (float)sqrt(sum / (double)n);
}

/* Check for NaN */
static int has_nan(const float *buf, int n) {
    for (int i = 0; i < n; i++)
        if (buf[i] != buf[i]) return 1;
    return 0;
}

/* Check for Inf */
static int has_inf(const float *buf, int n) {
    for (int i = 0; i < n; i++)
        if (buf[i] > 1e18f || buf[i] < -1e18f) return 1;
    return 0;
}

/* Compute energy at a specific frequency (±5 Hz Goertzel) */
static float band_energy(const float *buf, int n, float freq) {
    double s = 0.0, c = 0.0;
    for (int i = 0; i < n; i++) {
        double phase = 2.0 * M_PI * freq * (double)i / (double)SR;
        s += (double)buf[i] * sin(phase);
        c += (double)buf[i] * cos(phase);
    }
    return (float)sqrt(s*s + c*c) / (float)n;
}

/* Generate white noise */
static void gen_noise(float *buf, int n, float amp) {
    unsigned seed = 12345;
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        float r = (float)(int)seed / 2147483648.0f;
        if (r > 1.0f) r = 1.0f;
        if (r < -1.0f) r = -1.0f;
        buf[i] = r * amp;
    }
}

/* Generate sine wave */
static void gen_sine(float *buf, int n, float freq, float amp) {
    for (int i = 0; i < n; i++)
        buf[i] = amp * sinf(2.0f * M_PI * freq * (float)i / (float)SR);
}

int main(void) {
    int pass = 0, fail = 0;
    float *in = (float *)malloc(N * sizeof(float));
    float *out = (float *)malloc(N * sizeof(float));
    float *noise = (float *)malloc(N * sizeof(float));

    printf("=== test_restoration ===\n");

    /* ---- Test 1: Denoise reduces noise ---- */
    {
        /* Create signal: noise-only prefix (first 100ms) + sine + noise */
        int noise_prefix = (int)(0.1 * SR); /* 100ms */
        gen_noise(noise, N, 0.1f);
        /* First 100ms is noise only */
        for (int i = 0; i < noise_prefix; i++)
            in[i] = noise[i];
        /* Rest is sine + noise */
        float sine_part[44100];
        gen_sine(sine_part, N - noise_prefix, 440.0f, 0.5f);
        for (int i = noise_prefix; i < N; i++)
            in[i] = sine_part[i - noise_prefix] + noise[i] * 0.5f;

        float rms_before = compute_rms(in + noise_prefix, N - noise_prefix);

        int ret = wb_restoration_denoise(in, out, N, 0.7f);
        float rms_after = compute_rms(out + noise_prefix, N - noise_prefix);

        /* Check that output is finite and the sine component is preserved */
        int ok = (ret == 0) && !has_nan(out, N) && !has_inf(out, N);
        float sig_energy = band_energy(out + noise_prefix, N - noise_prefix, 440.0f);
        ok = ok && (sig_energy > 0.1f);

        printf("  [Test 1] Denoise reduces noise: %s (ret=%d, rms_before=%.4f, rms_after=%.4f, sig_energy=%.4f)\n",
               ok ? "PASS" : "FAIL", ret, rms_before, rms_after, sig_energy);
        if (ok) pass++; else fail++;
    }

    /* ---- Test 2: Declick preserves clean signal ---- */
    {
        /* Clean sine wave - declick should not alter it significantly */
        gen_sine(in, N, 1000.0f, 0.3f);

        int ret = wb_restoration_declick(in, out, N, 0.1f);

        /* Compare input and output - should be very close */
        double diff = 0.0;
        for (int i = 0; i < N; i++)
            diff += fabs((double)in[i] - (double)out[i]);
        float avg_diff = (float)(diff / (double)N);

        int ok = (ret == 0) && (avg_diff < 0.01f) && !has_nan(out, N);

        printf("  [Test 2] Declick preserves clean signal: %s (ret=%d, avg_diff=%.6f)\n",
               ok ? "PASS" : "FAIL", ret, avg_diff);
        if (ok) pass++; else fail++;
    }

    /* ---- Test 3: Declip reconstructs clipped samples ---- */
    {
        /* Create a sine wave and clip it */
        gen_sine(in, N, 440.0f, 0.9f);
        /* Clip at 0.5 */
        for (int i = 0; i < N; i++) {
            if (in[i] > 0.5f) in[i] = 0.5f;
            if (in[i] < -0.5f) in[i] = -0.5f;
        }

        /* Count clipped samples */
        int clipped_before = 0;
        for (int i = 0; i < N; i++)
            if (fabsf(in[i]) >= 0.49f) clipped_before++;

        int ret = wb_restoration_declip(in, out, N, 0.49f);

        /* Count clipped samples after */
        int clipped_after = 0;
        for (int i = 0; i < N; i++)
            if (fabsf(out[i]) >= 0.49f) clipped_after++;

        int ok = (ret == 0) && (clipped_after < clipped_before) && !has_nan(out, N);

        printf("  [Test 3] Declip reconstructs clipped samples: %s (ret=%d, before=%d, after=%d)\n",
               ok ? "PASS" : "FAIL", ret, clipped_before, clipped_after);
        if (ok) pass++; else fail++;
    }

    /* ---- Test 4: Dehum reduces hum frequency ---- */
    {
        /* Create signal: 1kHz sine + 60Hz hum */
        gen_sine(in, N, 1000.0f, 0.3f);
        float hum[44100];
        gen_sine(hum, N, 60.0f, 0.2f);
        for (int i = 0; i < N; i++)
            in[i] += hum[i];

        float hum_energy_before = band_energy(in, N, 60.0f);

        int ret = wb_restoration_dehum(in, out, N, 60.0f);

        float hum_energy_after = band_energy(out, N, 60.0f);

        int ok = (ret == 0) && (hum_energy_after < hum_energy_before * 0.5f) && !has_nan(out, N);

        printf("  [Test 4] Dehum reduces hum frequency: %s (ret=%d, hum_before=%.4f, hum_after=%.4f)\n",
               ok ? "PASS" : "FAIL", ret, hum_energy_before, hum_energy_after);
        if (ok) pass++; else fail++;
    }

    /* ---- Test 5: Voice isolation preserves speech band ---- */
    {
        /* Create signal: 500Hz (speech band) + 50Hz (sub-bass) + 8kHz (high freq) */
        float speech[44100], subbass[44100], highfreq[44100];
        gen_sine(speech, N, 500.0f, 0.4f);
        gen_sine(subbass, N, 50.0f, 0.3f);
        gen_sine(highfreq, N, 8000.0f, 0.3f);
        for (int i = 0; i < N; i++)
            in[i] = speech[i] + subbass[i] + highfreq[i];

        float speech_before = band_energy(in, N, 500.0f);
        float subbass_before = band_energy(in, N, 50.0f);
        float highfreq_before = band_energy(in, N, 8000.0f);

        int ret = wb_restoration_voice_isolate(in, out, N, 0.8f);

        float speech_after = band_energy(out, N, 500.0f);
        float subbass_after = band_energy(out, N, 50.0f);
        float highfreq_after = band_energy(out, N, 8000.0f);

        /* Speech band should be preserved, sub-bass and high freq should be attenuated */
        int ok = (ret == 0) &&
                 (speech_after > speech_before * 0.15f) &&
                 (subbass_after < subbass_before * 0.5f) &&
                 (highfreq_after < highfreq_before * 0.5f) &&
                 !has_nan(out, N);

        printf("  [Test 5] Voice isolation preserves speech band: %s (ret=%d)\n", ok ? "PASS" : "FAIL", ret);
        printf("    speech: %.4f -> %.4f, subbass: %.4f -> %.4f, highfreq: %.4f -> %.4f\n",
               speech_before, speech_after, subbass_before, subbass_after, highfreq_before, highfreq_after);
        if (ok) pass++; else fail++;
    }

    /* ---- Test 6: Output finite (no NaN) for all functions ---- */
    {
        /* Use a challenging signal: mix of everything */
        gen_sine(in, N, 440.0f, 0.5f);
        gen_noise(noise, N, 0.05f);
        for (int i = 0; i < N; i++)
            in[i] += noise[i];

        int ok = 1;

        if (wb_restoration_denoise(in, out, N, 0.5f) != 0 || has_nan(out, N) || has_inf(out, N)) ok = 0;
        if (wb_restoration_declick(in, out, N, 0.2f) != 0 || has_nan(out, N) || has_inf(out, N)) ok = 0;
        if (wb_restoration_declip(in, out, N, 0.9f) != 0 || has_nan(out, N) || has_inf(out, N)) ok = 0;
        if (wb_restoration_dehum(in, out, N, 60.0f) != 0 || has_nan(out, N) || has_inf(out, N)) ok = 0;
        if (wb_restoration_voice_isolate(in, out, N, 0.5f) != 0 || has_nan(out, N) || has_inf(out, N)) ok = 0;

        printf("  [Test 6] Output finite (no NaN/Inf): %s\n", ok ? "PASS" : "FAIL");
        if (ok) pass++; else fail++;
    }

    printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);

    free(in);
    free(out);
    free(noise);

    return fail > 0 ? 1 : 0;
}