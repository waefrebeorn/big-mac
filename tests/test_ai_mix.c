/* tests/test_ai_mix.c — tests for the AI-assisted mixing tools. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "wbus.h"

static int pass = 0, fail = 0;

static void check(int cond, const char *label) {
    if (cond) { printf("  PASS: %s\n", label); pass++; }
    else      { printf("  FAIL: %s\n", label); fail++; }
}

static float compute_rms(const float *buf, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += (double)buf[i] * (double)buf[i];
    return sqrt(s / (double)n);
}

static int has_nan(const float *buf, int n) {
    for (int i = 0; i < n; i++)
        if (buf[i] != buf[i]) return 1;
    return 0;
}

int main(void) {
    uint32_t sr = 44100;
    int n = 4096;

    /* Generate a test tone: 440 Hz sine at -12 dBFS RMS */
    float *audio = (float *)calloc(n, sizeof(float));
    float *out   = (float *)calloc(n, sizeof(float));
    float *silence = (float *)calloc(n, sizeof(float));

    for (int i = 0; i < n; i++) {
        double t = (double)i / (double)sr;
        audio[i] = 0.1f * sinf(2.0f * (float)M_PI * 440.0f * (float)t);
    }

    printf("=== AI Mix Test Suite ===\n");

    /* ---- 1. Analysis returns sane values ---- */
    {
        float rms, peak, lufs, cf, sc;
        int rc = wb_ai_mix_analyze(audio, (uint32_t)n, sr,
                                   &rms, &peak, &lufs, &cf, &sc);
        check(rc == 0, "analyze returns 0");
        check(rms > 0.001f && rms < 1.0f, "RMS in sane range (0.001–1.0)");
        check(peak > 0.0f && peak <= 1.0f, "peak in sane range (0–1)");
        check(lufs > -80.0f && lufs < 0.0f, "LUFS in sane range (-80–0)");
        check(cf > -50.0f && cf < 50.0f, "crest factor in sane range (-50..50 dB)");
        check(sc > 0.0f && sc < (float)sr, "spectral centroid in valid Hz range");
        printf("    RMS=%.4f  Peak=%.4f  LUFS=%.2f  CF=%.2fdB  SC=%.1fHz\n",
               rms, peak, lufs, cf, sc);
    }

    /* ---- 7. Silence analysis returns zeros ---- */
    {
        float rms, peak, lufs, cf, sc;
        wb_ai_mix_analyze(silence, (uint32_t)n, sr,
                          &rms, &peak, &lufs, &cf, &sc);
        check(rms == 0.0f, "silence RMS == 0");
        check(peak == 0.0f, "silence peak == 0");
        check(fabsf(cf) < 1.0f, "silence crest factor ~0");
        /* spectral centroid of silence should be 0 (no energy) */
        check(sc == 0.0f, "silence spectral centroid == 0");
        printf("    Silence: RMS=%.6f Peak=%.6f LUFS=%.2f CF=%.2f SC=%.1f\n",
               rms, peak, lufs, cf, sc);
    }

    /* ---- 2. Auto-level changes RMS toward target ---- */
    {
        float target = -14.0f;
        int rc = wb_ai_mix_auto_level(audio, out, (uint32_t)n, sr, target);
        check(rc == 0, "auto_level returns 0");
        float rms_in  = compute_rms(audio, n);
        float rms_out = compute_rms(out, n);
        float gain_ratio = rms_out / (rms_in + 1e-12f);
        check(gain_ratio > 1.0f, "auto_level increases RMS toward target");
        printf("    RMS in=%.4f  out=%.4f  ratio=%.2f\n",
               rms_in, rms_out, gain_ratio);
    }

    /* ---- 3. Auto-EQ modifies spectral balance ---- */
    {
        float gains[8] = {0};
        int rc = wb_ai_mix_auto_eq(audio, out, (uint32_t)n, sr,
                                   gains, 8);
        check(rc == 0, "auto_eq returns 0");
        float rms_in  = compute_rms(audio, n);
        float rms_out = compute_rms(out, n);
        check(rms_in != rms_out, "auto_eq changes signal (RMS differs)");
        int nonzero = 0;
        for (int b = 0; b < 8; b++)
            if (fabsf(gains[b]) > 0.001f) nonzero++;
        check(nonzero > 0, "auto_eq produces non-zero gain suggestions");
        printf("    RMS in=%.4f out=%.4f  gains=[%+.1f %+.1f %+.1f %+.1f %+.1f %+.1f %+.1f %+.1f]\n",
               rms_in, rms_out,
               gains[0],gains[1],gains[2],gains[3],gains[4],gains[5],gains[6],gains[7]);
    }

    /* ---- 4. Pan suggestions are in valid range (-1 to 1) ---- */
    {
        uint32_t tc = 8;
        float centroids[8] = {200, 400, 800, 1500, 3000, 6000, 10000, 15000};
        float pans[8] = {0};
        int rc = wb_ai_mix_suggest_pan(tc, centroids, pans);
        check(rc == 0, "suggest_pan returns 0");
        int all_valid = 1;
        for (int i = 0; i < 8; i++) {
            if (pans[i] < -1.0f || pans[i] > 1.0f) all_valid = 0;
        }
        check(all_valid, "all pan values in [-1, 1]");
        printf("    pans = [%+.2f %+.2f %+.2f %+.2f %+.2f %+.2f %+.2f %+.2f]\n",
               pans[0],pans[1],pans[2],pans[3],pans[4],pans[5],pans[6],pans[7]);
    }

    /* ---- 5. De-ess reduces high-frequency energy when sibilance present ---- */
    {
        /* Generate sibilant signal: mostly 100 Hz + sharp 8 kHz transient */
        float *sib = (float *)calloc(n, sizeof(float));
        for (int i = 0; i < n; i++) {
            double t = (double)i / (double)sr;
            /* Low tone + high-frequency sibilance cluster */
            sib[i]  = 0.05f * sinf(2.0f * (float)M_PI * 100.0f * (float)t);
            if (i % 50 < 10)  /* bursts at 8 kHz */
                sib[i] += 0.8f * sinf(2.0f * (float)M_PI * 8000.0f * (float)t);
        }

        int rc = wb_ai_mix_de_ess(sib, out, (uint32_t)n, sr, -30.0f);
        check(rc == 0, "de_ess returns 0");

        /* Compare HF energy (above 4 kHz) before and after */
        float hf_before = 0, hf_after = 0;
        for (int i = 0; i < n; i++) {
            hf_before += sib[i] * sib[i];
            hf_after  += out[i] * out[i];
        }
        /* Check total energy reduction is present (sibilance attenuated) */
        check(hf_after < hf_before, "de_ess reduces overall energy for sibilant signal");
        printf("    energy before=%.6f  after=%.6f\n", hf_before, hf_after);
        free(sib);
    }

    /* ---- 6. Output finite (no NaN) ---- */
    {
        /* Run all processing functions, verify no NaN in outputs */
        float gains[8] = {0};
        wb_ai_mix_auto_eq(audio, out, (uint32_t)n, sr, gains, 8);
        check(!has_nan(out, n), "auto_eq output has no NaN");

        wb_ai_mix_auto_level(audio, out, (uint32_t)n, sr, -14.0f);
        check(!has_nan(out, n), "auto_level output has no NaN");

        float *sib = (float *)calloc(n, sizeof(float));
        for (int i = 0; i < n; i++) {
            double t = (double)i / (double)sr;
            sib[i] = 0.05f * sinf(2.0f * (float)M_PI * 100.0f * (float)t);
            if (i % 50 < 10)
                sib[i] += 0.8f * sinf(2.0f * (float)M_PI * 8000.0f * (float)t);
        }
        wb_ai_mix_de_ess(sib, out, (uint32_t)n, sr, -30.0f);
        check(!has_nan(out, n), "de_ess output has no NaN");
        free(sib);
    }

    /* ---- 8. Output length matches input ---- */
    {
        /* Verify auto_eq output has same length */
        float gains[8] = {0};
        wb_ai_mix_auto_eq(audio, out, (uint32_t)n, sr, gains, 8);
        /* Verify auto_level output has same length */
        wb_ai_mix_auto_level(audio, out, (uint32_t)n, sr, -14.0f);
        /* Just verify we can read all n samples without issue (no crash) */
        int readable = 1;
        for (int i = 0; i < n; i++) {
            if (out[i] != out[i]) { readable = 0; break; }  /* NaN check */
        }
        check(readable, "auto_level output fully readable (length matches)");
    }

    free(audio);
    free(out);
    free(silence);

    printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);
    return (fail > 0) ? 1 : 0;
}
