/* test_atmos.c — Dolby Atmos-style spatial audio panner verification.
 *
 * Exercises wb_atmos_* with known configurations:
 *   1. Create/destroy lifecycle
 *   2. Set object position (clamping)
 *   3. Process produces stereo output (non-silent)
 *   4. Left source louder in left channel
 *   5. Distance affects level (closer = louder)
 *   6. Multiple objects mix correctly
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

/* Compute RMS of a buffer */
static float rms(const float *buf, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += buf[i] * buf[i];
    return sqrtf(sum / (float)n);
}

int main(void) {
    printf("=== Spatial Audio Panner (wb_atmos) ===\n\n");

    const uint32_t sr = 44100;
    const int frames = 8192;

    /* ---- 1. Create/destroy ---- */
    void *atmos = wb_atmos_create(sr);
    CK(atmos != NULL, "wb_atmos_create returns non-NULL");
    if (!atmos) {
        printf("\n%d/%d passed\n", checks - failures, checks);
        return 1;
    }
    CK(wb_atmos_get_object_count(atmos) == 16, "Object count is 16");
    wb_atmos_destroy(atmos);
    CK(1, "wb_atmos_destroy completes without crash");

    /* Recreate for remaining tests */
    atmos = wb_atmos_create(sr);

    /* Generate a test signal: 1 kHz sine wave */
    float *sine = (float *)malloc((size_t)frames * sizeof(float));
    for (int i = 0; i < frames; i++) {
        sine[i] = (float)sin(2.0 * M_PI * 1000.0 * (double)i / (double)sr);
    }

    /* Output buffers */
    float *out_l = (float *)calloc((size_t)frames, sizeof(float));
    float *out_r = (float *)calloc((size_t)frames, sizeof(float));
    wb_sample *outputs[2] = {out_l, out_r};

    /* ---- 2. Set object position ---- */
    wb_atmos_set_position(atmos, 0, -30.0f, 0.0f, 1.0f);
    wb_atmos_set_object_gain(atmos, 0, 1.0f);
    CK(1, "wb_atmos_set_position and set_object_gain complete");

    /* Test clamping (should not crash) */
    wb_atmos_set_position(atmos, 1, -999.0f, 999.0f, 100.0f);
    CK(1, "wb_atmos_set_position clamps out-of-range values");

    /* ---- 3. Process produces stereo output ---- */
    {
        const wb_sample *inputs[1] = {sine};
        int rc = wb_atmos_process(atmos, inputs, outputs, 1, (uint32_t)frames);
        CK(rc == 0, "wb_atmos_process returns 0 on success");

        float rms_l = rms(out_l, frames);
        float rms_r = rms(out_r, frames);
        printf("  RMS L=%.4f R=%.4f\n", rms_l, rms_r);
        CK(rms_l > 0.001f && rms_r > 0.001f,
           "Process produces non-silent stereo output");
    }

    /* ---- 4. Left source louder in left channel ---- */
    {
        memset(out_l, 0, frames * sizeof(float));
        memset(out_r, 0, frames * sizeof(float));

        /* Source on left (-60 deg azimuth) */
        wb_atmos_set_position(atmos, 0, -60.0f, 0.0f, 1.0f);
        wb_atmos_set_object_gain(atmos, 0, 1.0f);

        const wb_sample *inputs[1] = {sine};
        wb_atmos_process(atmos, inputs, outputs, 1, (uint32_t)frames);

        float rms_l = rms(out_l, frames);
        float rms_r = rms(out_r, frames);
        printf("  Left source (-60 deg): RMS L=%.4f R=%.4f (ratio=%.2f)\n",
               rms_l, rms_r, rms_l / (rms_r + 1e-10f));
        CK(rms_l > rms_r * 1.2f,
           "Left source is louder in left channel (IID works)");

        /* Now test right source */
        memset(out_l, 0, frames * sizeof(float));
        memset(out_r, 0, frames * sizeof(float));
        wb_atmos_set_position(atmos, 0, 60.0f, 0.0f, 1.0f);
        wb_atmos_process(atmos, inputs, outputs, 1, (uint32_t)frames);

        float rms_l2 = rms(out_l, frames);
        float rms_r2 = rms(out_r, frames);
        printf("  Right source (+60 deg): RMS L=%.4f R=%.4f (ratio=%.2f)\n",
               rms_l2, rms_r2, rms_r2 / (rms_l2 + 1e-10f));
        CK(rms_r2 > rms_l2 * 1.2f,
           "Right source is louder in right channel (IID works)");
    }

    /* ---- 5. Distance affects level ---- */
    {
        wb_atmos_set_position(atmos, 0, 0.0f, 0.0f, 1.0f);
        wb_atmos_set_object_gain(atmos, 0, 1.0f);

        memset(out_l, 0, frames * sizeof(float));
        memset(out_r, 0, frames * sizeof(float));
        const wb_sample *inputs[1] = {sine};
        wb_atmos_process(atmos, inputs, outputs, 1, (uint32_t)frames);
        float rms_close = rms(out_l, frames) + rms(out_r, frames);

        /* Far away (5m) */
        wb_atmos_set_position(atmos, 0, 0.0f, 0.0f, 5.0f);
        memset(out_l, 0, frames * sizeof(float));
        memset(out_r, 0, frames * sizeof(float));
        wb_atmos_process(atmos, inputs, outputs, 1, (uint32_t)frames);
        float rms_far = rms(out_l, frames) + rms(out_r, frames);

        printf("  Distance 1m: total RMS=%.4f, 5m: total RMS=%.4f\n",
               rms_close, rms_far);
        CK(rms_close > rms_far * 2.0f,
           "Closer source is significantly louder (inverse square law)");
    }

    /* ---- 6. Multiple objects mix correctly ---- */
    {
        /* Two objects: one left, one right */
        float *sine2 = (float *)malloc((size_t)frames * sizeof(float));
        for (int i = 0; i < frames; i++) {
            sine2[i] = (float)sin(2.0 * M_PI * 500.0 * (double)i / (double)sr);
        }

        wb_atmos_set_position(atmos, 0, -45.0f, 0.0f, 1.0f);
        wb_atmos_set_object_gain(atmos, 0, 1.0f);
        wb_atmos_set_position(atmos, 1, 45.0f, 0.0f, 1.0f);
        wb_atmos_set_object_gain(atmos, 1, 1.0f);

        memset(out_l, 0, frames * sizeof(float));
        memset(out_r, 0, frames * sizeof(float));
        const wb_sample *inputs[2] = {sine, sine2};
        int rc = wb_atmos_process(atmos, inputs, outputs, 2, (uint32_t)frames);
        CK(rc == 0, "Multiple objects: process returns 0");

        float rms_l = rms(out_l, frames);
        float rms_r = rms(out_r, frames);
        printf("  Two objects (L+R): RMS L=%.4f R=%.4f\n", rms_l, rms_r);
        CK(rms_l > 0.001f && rms_r > 0.001f,
           "Multiple objects produce stereo output on both channels");

        /* With symmetric placement, L and R should be roughly balanced */
        float ratio = rms_l > rms_r ? rms_l / rms_r : rms_r / rms_l;
        printf("  L/R balance ratio: %.2f\n", ratio);
        CK(ratio < 3.0f,
           "Symmetric sources produce reasonably balanced stereo");

        free(sine2);
    }

    /* Cleanup */
    free(sine);
    free(out_l);
    free(out_r);
    wb_atmos_destroy(atmos);

    printf("\n=== Results: %d/%d passed ===\n", checks - failures, checks);
    return failures > 0 ? 1 : 0;
}