/* test_analysis.c — broadcast/metering audio analysis suite verification.
 *
 * Exercises wb_analysis_* with known signals:
 *   1. Loudness returns sane value (full-scale sine ~ -3 LUFS)
 *   2. Peak detection (full-scale sine = 0 dBFS)
 *   3. RMS computation (full-scale sine = -3.01 dBFS)
 *   4. Crest factor (full-scale sine = 3.01 dB)
 *   5. Spectrum has energy (sine peak in correct band)
 *   6. Phase correlation for mono signal = 1.0
 *   7. Phase correlation for anti-phase = -1.0
 *   8. Null input returns error
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus/wbus.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures = 0, checks = 0;
#define CK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

static float absf(float x) { return x >= 0.0f ? x : -x; }

int main(void) {
    printf("=== Audio analysis suite ===\n\n");

    const float sr = 44100.0f;
    const int n = 44100; /* 1 second */

    /* Generate a full-scale 1 kHz sine wave */
    float *sine = (float*)malloc((size_t)n * sizeof(float));
    for (int i = 0; i < n; i++) {
        sine[i] = (float)sin(2.0 * M_PI * 1000.0 * i / sr);
    }

    /* ---- 1. Loudness returns sane value ---- */
    {
        float lufs;
        int rc = wb_analysis_loudness(sine, n, sr, &lufs);
        printf("  Loudness: %.2f LUFS (rc=%d)\n", lufs, rc);
        CK(rc == 0 && lufs > -20.0f && lufs < 0.0f,
           "Loudness returns sane value for full-scale sine");
    }

    /* ---- 2. Peak detection ---- */
    {
        float peak_db;
        int rc = wb_analysis_peak(sine, n, &peak_db);
        printf("  Peak: %.2f dBFS (rc=%d)\n", peak_db, rc);
        CK(rc == 0 && absf(peak_db - 0.0f) < 0.1f,
           "Peak detection: full-scale sine ~ 0 dBFS");
    }

    /* ---- 3. RMS computation ---- */
    {
        float rms_db;
        int rc = wb_analysis_rms(sine, n, &rms_db);
        printf("  RMS: %.2f dBFS (rc=%d)\n", rms_db, rc);
        CK(rc == 0 && absf(rms_db - (-3.01f)) < 0.2f,
           "RMS computation: full-scale sine ~ -3.01 dBFS");
    }

    /* ---- 4. Crest factor ---- */
    {
        float crest_db;
        int rc = wb_analysis_crest_factor(sine, n, &crest_db);
        printf("  Crest factor: %.2f dB (rc=%d)\n", crest_db, rc);
        CK(rc == 0 && absf(crest_db - 3.01f) < 0.3f,
           "Crest factor: full-scale sine ~ 3.01 dB");
    }

    /* ---- 5. Spectrum has energy ---- */
    {
        int num_bins = 32;
        float *bins = (float*)malloc((size_t)num_bins * sizeof(float));
        int rc = wb_analysis_spectrum(sine, n, bins, num_bins, sr);
        /* Find peak bin */
        int peak_bin = 0;
        float peak_val = bins[0];
        for (int b = 1; b < num_bins; b++) {
            if (bins[b] > peak_val) { peak_val = bins[b]; peak_bin = b; }
        }
        printf("  Spectrum peak bin: %d (%.1f dB)\n", peak_bin, peak_val);
        CK(rc == 0 && peak_val > -30.0f,
           "Spectrum has energy above noise floor");
        /* 1 kHz should be in a mid-range bin (not the lowest or highest) */
        CK(peak_bin > 0 && peak_bin < num_bins - 1,
           "Spectrum peak in expected mid-range band for 1 kHz");
        free(bins);
    }

    /* ---- 6. Phase correlation for mono signal = 1.0 ---- */
    {
        float corr;
        int rc = wb_analysis_phase_correlation(sine, sine, n, &corr);
        printf("  Phase correlation (mono): %.4f (rc=%d)\n", corr, rc);
        CK(rc == 0 && absf(corr - 1.0f) < 0.01f,
           "Phase correlation for identical signals = 1.0");
    }

    /* ---- 7. Phase correlation for anti-phase = -1.0 ---- */
    {
        float *anti = (float*)malloc((size_t)n * sizeof(float));
        for (int i = 0; i < n; i++) anti[i] = -sine[i];
        float corr;
        int rc = wb_analysis_phase_correlation(sine, anti, n, &corr);
        printf("  Phase correlation (anti): %.4f (rc=%d)\n", corr, rc);
        CK(rc == 0 && absf(corr - (-1.0f)) < 0.01f,
           "Phase correlation for anti-phase signals = -1.0");
        free(anti);
    }

    /* ---- 8. Null input returns error ---- */
    {
        float val;
        int rc = wb_analysis_peak(NULL, n, &val);
        CK(rc == -1, "NULL input returns error code");
    }

    free(sine);

    printf("\n--- %d/%d checks passed ---\n", checks - failures, checks);
    return failures > 0 ? 1 : 0;
}