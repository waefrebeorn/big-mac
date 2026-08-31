/* tests/test_sonogram.c — sonogram / spectrogram visualization test.
 * Pure C11, standalone: only needs wb_sonogram.o + wb_fft.o.
 *
 * Tests:
 *   1. Create/destroy
 *   2. Process audio (sine wave)
 *   3. Render produces non-zero pixels
 *   4. Peak detection works
 *   5. RMS computation correct
 *   6. Spectral centroid in valid range
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wbus/wbus_sonogram.h"

#define SR 44100

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass_count++; } \
    else      { printf("  FAIL: %s\n", msg); fail_count++; } \
} while (0)

int main(void) {
    printf("=== test_sonogram ===\n");

    /* ---- Test 1: Create/destroy ---- */
    {
        wb_sonogram *sg = wb_sonogram_create(SR, 256, 128);
        CHECK(sg != NULL, "wb_sonogram_create returns non-NULL");
        wb_sonogram_destroy(sg);
        CHECK(1, "wb_sonogram_destroy completes without crash");
    }

    /* ---- Test 2: Process audio ---- */
    {
        wb_sonogram *sg = wb_sonogram_create(SR, 256, 128);
        CHECK(sg != NULL, "create for process test");

        /* Generate 1 second of 440 Hz sine wave */
        uint32_t frames = SR;
        float *audio = (float *)malloc(frames * sizeof(float));
        CHECK(audio != NULL, "allocate test audio buffer");

        for (uint32_t i = 0; i < frames; i++) {
            audio[i] = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)SR);
        }

        int rc = wb_sonogram_process(sg, audio, frames);
        CHECK(rc == 0, "wb_sonogram_process returns 0 on success");

        /* Process more audio (another second) to fill columns */
        rc = wb_sonogram_process(sg, audio, frames);
        CHECK(rc == 0, "wb_sonogram_process second call returns 0");

        free(audio);
        wb_sonogram_destroy(sg);
        CHECK(1, "cleanup after process test");
    }

    /* ---- Test 3: Render produces non-zero pixels ---- */
    {
        wb_sonogram *sg = wb_sonogram_create(SR, 128, 64);
        CHECK(sg != NULL, "create for render test");

        /* Generate 0.5s of 1000 Hz sine */
        uint32_t frames = SR / 2;
        float *audio = (float *)malloc(frames * sizeof(float));
        CHECK(audio != NULL, "allocate render test audio");

        for (uint32_t i = 0; i < frames; i++) {
            audio[i] = 0.8f * sinf(2.0f * 3.14159265f * 1000.0f * (float)i / (float)SR);
        }

        wb_sonogram_process(sg, audio, frames);

        int width = 128, height = 64;
        uint8_t *rgba = (uint8_t *)calloc(width * height * 4, 1);
        CHECK(rgba != NULL, "allocate RGBA buffer");

        int rc = wb_sonogram_render(sg, rgba, width, height);
        CHECK(rc == 0, "wb_sonogram_render returns 0");

        /* Count non-zero pixels */
        int nonzero = 0;
        for (int i = 0; i < width * height * 4; i += 4) {
            if (rgba[i] != 0 || rgba[i+1] != 0 || rgba[i+2] != 0) {
                nonzero++;
            }
        }
        CHECK(nonzero > 0, "render produces non-zero (colored) pixels");
        printf("    (non-zero pixel count: %d)\n", nonzero);

        free(rgba);
        free(audio);
        wb_sonogram_destroy(sg);
    }

    /* ---- Test 4: Peak detection ---- */
    {
        wb_sonogram *sg = wb_sonogram_create(SR, 64, 32);
        CHECK(sg != NULL, "create for peak test");

        /* Generate a signal with known peak = 0.75 */
        uint32_t frames = 4410;  /* 100ms */
        float *audio = (float *)malloc(frames * sizeof(float));
        CHECK(audio != NULL, "allocate peak test audio");

        float expected_peak = 0.75f;
        for (uint32_t i = 0; i < frames; i++) {
            audio[i] = expected_peak * sinf(2.0f * 3.14159265f * 220.0f * (float)i / (float)SR);
        }

        wb_sonogram_process(sg, audio, frames);

        float peak = wb_sonogram_get_peak(sg);
        CHECK(peak > 0.0f, "peak is non-zero after processing");
        /* Peak should be close to 0.75 (within 5% for a long enough sine) */
        float peak_err = fabsf(peak - expected_peak) / expected_peak;
        CHECK(peak_err < 0.05f, "peak value matches expected (within 5%)");
        printf("    (peak=%.4f, expected=%.4f, err=%.2f%%)\n", peak, expected_peak, peak_err * 100.0f);

        free(audio);
        wb_sonogram_destroy(sg);
    }

    /* ---- Test 5: RMS computation ---- */
    {
        wb_sonogram *sg = wb_sonogram_create(SR, 64, 32);
        CHECK(sg != NULL, "create for RMS test");

        /* Generate a sine with amplitude 0.6 — expected RMS = 0.6/sqrt(2) ≈ 0.4243 */
        uint32_t frames = SR;  /* 1 second for stable RMS */
        float *audio = (float *)malloc(frames * sizeof(float));
        CHECK(audio != NULL, "allocate RMS test audio");

        float amp = 0.6f;
        float expected_rms = amp / sqrtf(2.0f);
        for (uint32_t i = 0; i < frames; i++) {
            audio[i] = amp * sinf(2.0f * 3.14159265f * 330.0f * (float)i / (float)SR);
        }

        wb_sonogram_process(sg, audio, frames);

        float rms = wb_sonogram_get_rms(sg);
        CHECK(rms > 0.0f, "RMS is non-zero after processing");

        float rms_err = fabsf(rms - expected_rms) / expected_rms;
        CHECK(rms_err < 0.05f, "RMS value matches expected (within 5%)");
        printf("    (rms=%.4f, expected=%.4f, err=%.2f%%)\n", rms, expected_rms, rms_err * 100.0f);

        /* Also check crest factor: for a sine, crest = peak/rms = sqrt(2) ≈ 1.414 */
        float crest = wb_sonogram_get_crest_factor(sg);
        float expected_crest = sqrtf(2.0f);
        float crest_err = fabsf(crest - expected_crest) / expected_crest;
        CHECK(crest_err < 0.1f, "crest factor ≈ sqrt(2) for sine wave");
        printf("    (crest=%.4f, expected=%.4f, err=%.2f%%)\n", crest, expected_crest, crest_err * 100.0f);

        free(audio);
        wb_sonogram_destroy(sg);
    }

    /* ---- Test 6: Spectral centroid in valid range ---- */
    {
        wb_sonogram *sg = wb_sonogram_create(SR, 128, 64);
        CHECK(sg != NULL, "create for spectral centroid test");

        /* Generate 500 Hz sine — centroid should be near 500 Hz */
        uint32_t frames = SR * 2;
        float *audio = (float *)malloc(frames * sizeof(float));
        CHECK(audio != NULL, "allocate centroid test audio");

        float freq = 500.0f;
        for (uint32_t i = 0; i < frames; i++) {
            audio[i] = 0.7f * sinf(2.0f * 3.14159265f * freq * (float)i / (float)SR);
        }

        wb_sonogram_process(sg, audio, frames);

        float centroid = wb_sonogram_get_spectral_centroid(sg);
        CHECK(centroid > 0.0f, "spectral centroid is non-zero");

        /* Centroid should be between 0 and Nyquist */
        float nyquist = (float)SR / 2.0f;
        CHECK(centroid > 0.0f && centroid <= nyquist,
              "spectral centroid is in valid range [0, Nyquist]");

        /* For a pure sine at 500 Hz, centroid should be near 500 Hz */
        /* Allow wider tolerance due to spectral leakage and bin quantization */
        float centroid_err = fabsf(centroid - freq);
        CHECK(centroid_err < 100.0f,
              "spectral centroid near signal frequency (500 Hz ± 100 Hz)");
        printf("    (centroid=%.1f Hz, signal=%.0f Hz, err=%.1f Hz)\n",
               centroid, freq, centroid_err);

        free(audio);
        wb_sonogram_destroy(sg);
    }

    /* ---- Summary ---- */
    printf("\n=== test_sonogram: %d/%d passed ===\n",
           pass_count, pass_count + fail_count);

    return fail_count > 0 ? 1 : 0;
}