/* tests/test_spatial_audio.c — test HRTF binaural 3D audio panner. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "wbus.h"

static float compute_rms(const float *buf, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += buf[i] * buf[i];
    return sqrtf(sum / (float)n);
}

static int has_nan(const float *buf, int n) {
    for (int i = 0; i < n; i++)
        if (buf[i] != buf[i]) return 1;
    return 0;
}

/* Compute spectral centroid (mean freq via zero-crossing rate proxy) */
static float spectral_diff(const float *a, const float *b, int n) {
    float diff = 0;
    for (int i = 0; i < n; i++)
        diff += fabsf(a[i] - b[i]);
    return diff / (float)n;
}

int main(void) {
    int pass = 0, fail = 0;
    uint32_t sr = 44100;
    int n = 8820; /* 0.2s @ 44.1k */

    /* Generate a test signal: 1 kHz sine burst */
    float *in = (float *)calloc(n, sizeof(float));
    for (int i = 0; i < n; i++)
        in[i] = 0.5f * sinf(2.0f * (float)M_PI * 1000.0f * (float)i / (float)sr);

    float *out_l = (float *)calloc(n, sizeof(float));
    float *out_r = (float *)calloc(n, sizeof(float));
    float *out_l2 = (float *)calloc(n, sizeof(float));
    float *out_r2 = (float *)calloc(n, sizeof(float));

    /* 1. Create/destroy */
    void *sp = wb_spatial_create(sr);
    if (sp) { printf("  PASS: create\n"); pass++; }
    else { printf("  FAIL: create\n"); fail++; goto cleanup; }
    wb_spatial_destroy(sp);
    printf("  PASS: destroy\n"); pass++;

    /* Re-create for remaining tests */
    sp = wb_spatial_create(sr);

    /* 2. Source on left (-60°) → louder in left channel */
    wb_spatial_set_binaural(sp, 1);
    wb_spatial_set_position(sp, -60.0f, 0.0f, 1.0f);
    memset(out_l, 0, n * sizeof(float));
    memset(out_r, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l, out_r, n);
    {
        float rms_l = compute_rms(out_l + 100, n - 200); /* skip transient */
        float rms_r = compute_rms(out_r + 100, n - 200);
        if (rms_l > rms_r * 1.2f) { printf("  PASS: left source louder in L (L=%.4f R=%.4f)\n", rms_l, rms_r); pass++; }
        else { printf("  FAIL: left source not louder in L (L=%.4f R=%.4f)\n", rms_l, rms_r); fail++; }
    }

    /* 3. Source on right (+60°) → louder in right channel */
    wb_spatial_set_position(sp, 60.0f, 0.0f, 1.0f);
    memset(out_l, 0, n * sizeof(float));
    memset(out_r, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l, out_r, n);
    {
        float rms_l = compute_rms(out_l + 100, n - 200);
        float rms_r = compute_rms(out_r + 100, n - 200);
        if (rms_r > rms_l * 1.2f) { printf("  PASS: right source louder in R (L=%.4f R=%.4f)\n", rms_l, rms_r); pass++; }
        else { printf("  FAIL: right source not louder in R (L=%.4f R=%.4f)\n", rms_l, rms_r); fail++; }
    }

    /* 4. Source in center (0°) → equal L/R */
    wb_spatial_set_position(sp, 0.0f, 0.0f, 1.0f);
    memset(out_l, 0, n * sizeof(float));
    memset(out_r, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l, out_r, n);
    {
        float rms_l = compute_rms(out_l + 100, n - 200);
        float rms_r = compute_rms(out_r + 100, n - 200);
        float ratio = (rms_l > rms_r) ? rms_l / (rms_r + 1e-10f) : rms_r / (rms_l + 1e-10f);
        if (ratio < 1.15f) { printf("  PASS: center source equal L/R (ratio=%.3f)\n", ratio); pass++; }
        else { printf("  FAIL: center source not equal (L=%.4f R=%.4f ratio=%.3f)\n", rms_l, rms_r, ratio); fail++; }
    }

    /* 5. Distance affects level (closer = louder) */
    wb_spatial_set_position(sp, 0.0f, 0.0f, 1.0f);
    memset(out_l, 0, n * sizeof(float));
    memset(out_r, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l, out_r, n);
    float rms_close = compute_rms(out_l + 100, n - 200);

    wb_spatial_set_position(sp, 0.0f, 0.0f, 5.0f);
    memset(out_l, 0, n * sizeof(float));
    memset(out_r, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l, out_r, n);
    float rms_far = compute_rms(out_l + 100, n - 200);

    if (rms_close > rms_far * 2.0f) {
        printf("  PASS: distance affects level (close=%.4f far=%.4f ratio=%.1fx)\n",
               rms_close, rms_far, rms_close / (rms_far + 1e-10f));
        pass++;
    } else {
        printf("  FAIL: distance effect too weak (close=%.4f far=%.4f)\n", rms_close, rms_far);
        fail++;
    }

    /* 6. Elevation changes spectral content (pinna notch) */
    wb_spatial_set_position(sp, 30.0f, 0.0f, 1.0f);
    memset(out_l, 0, n * sizeof(float));
    memset(out_r, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l, out_r, n);
    float rms_el0 = compute_rms(out_l + 100, n - 200);

    wb_spatial_set_position(sp, 30.0f, 60.0f, 1.0f);
    memset(out_l2, 0, n * sizeof(float));
    memset(out_r2, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l2, out_r2, n);
    float rms_el60 = compute_rms(out_l2 + 100, n - 200);

    /* The notch filter changes the output — check they differ */
    float el_diff = spectral_diff(out_l, out_l2, n - 200);
    if (el_diff > 0.001f || fabsf(rms_el0 - rms_el60) > 0.001f) {
        printf("  PASS: elevation changes output (diff=%.5f rms0=%.4f rms60=%.4f)\n",
               el_diff, rms_el0, rms_el60);
        pass++;
    } else {
        printf("  FAIL: elevation has no effect (diff=%.5f)\n", el_diff);
        fail++;
    }

    /* 7. Output finite (no NaN/Inf) */
    wb_spatial_set_position(sp, -45.0f, 30.0f, 2.0f);
    memset(out_l, 0, n * sizeof(float));
    memset(out_r, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l, out_r, n);
    if (!has_nan(out_l, n) && !has_nan(out_r, n)) {
        printf("  PASS: output finite (no NaN)\n");
        pass++;
    } else {
        printf("  FAIL: output contains NaN\n");
        fail++;
    }

    /* 8. Binaural vs non-binaural mode produce different output */
    wb_spatial_set_binaural(sp, 1);
    wb_spatial_set_position(sp, -30.0f, 10.0f, 1.5f);
    memset(out_l, 0, n * sizeof(float));
    memset(out_r, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l, out_r, n);
    float rms_binaural_l = compute_rms(out_l + 100, n - 200);

    wb_spatial_set_binaural(sp, 0);
    memset(out_l2, 0, n * sizeof(float));
    memset(out_r2, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l2, out_r2, n);
    float rms_vbap_l = compute_rms(out_l2 + 100, n - 200);

    /* They should differ because binaural has ITD/IID/notch, VBAP doesn't */
    float mode_diff = spectral_diff(out_l, out_l2, n - 200);
    if (mode_diff > 0.0005f || fabsf(rms_binaural_l - rms_vbap_l) > 0.001f) {
        printf("  PASS: binaural vs non-binaural differ (diff=%.5f binaural=%.4f vbap=%.4f)\n",
               mode_diff, rms_binaural_l, rms_vbap_l);
        pass++;
    } else {
        printf("  FAIL: modes produce identical output (diff=%.5f)\n", mode_diff);
        fail++;
    }

    /* 9. Listener orientation affects output (bonus) */
    wb_spatial_set_binaural(sp, 1);
    wb_spatial_set_listener_orientation(sp, 0.0f, 0.0f, 0.0f);
    wb_spatial_set_position(sp, 0.0f, 0.0f, 1.0f);
    memset(out_l, 0, n * sizeof(float));
    memset(out_r, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l, out_r, n);

    wb_spatial_set_listener_orientation(sp, 45.0f, 0.0f, 0.0f);
    memset(out_l2, 0, n * sizeof(float));
    memset(out_r2, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l2, out_r2, n);

    float orient_diff = spectral_diff(out_l, out_l2, n - 200);
    if (orient_diff > 0.0001f) {
        printf("  PASS: listener orientation affects output (diff=%.5f)\n", orient_diff);
        pass++;
    } else {
        printf("  FAIL: orientation has no effect (diff=%.5f)\n", orient_diff);
        fail++;
    }

    /* 10. Room reverb adds energy (bonus) */
    wb_spatial_set_listener_orientation(sp, 0.0f, 0.0f, 0.0f);
    wb_spatial_set_room(sp, 0.0f, 5.0f);
    wb_spatial_set_position(sp, 0.0f, 0.0f, 1.0f);
    memset(out_l, 0, n * sizeof(float));
    memset(out_r, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l, out_r, n);
    float rms_no_reverb = compute_rms(out_l + 100, n - 200);

    wb_spatial_set_room(sp, 0.5f, 5.0f);
    memset(out_l2, 0, n * sizeof(float));
    memset(out_r2, 0, n * sizeof(float));
    wb_spatial_process(sp, in, out_l2, out_r2, n);
    float rms_with_reverb = compute_rms(out_l2 + 100, n - 200);

    if (rms_with_reverb > rms_no_reverb) {
        printf("  PASS: room reverb adds energy (no=%.4f with=%.4f)\n", rms_no_reverb, rms_with_reverb);
        pass++;
    } else {
        printf("  FAIL: reverb didn't add energy (no=%.4f with=%.4f)\n", rms_no_reverb, rms_with_reverb);
        fail++;
    }

    wb_spatial_destroy(sp);

cleanup:
    free(in); free(out_l); free(out_r); free(out_l2); free(out_r2);
    printf("\n  === %d/%d checks passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}