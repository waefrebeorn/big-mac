/* tests/test_wavetable.c — test wavetable synthesis feature. */
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

int main(void) {
    int pass = 0, fail = 0;
    uint32_t sr = 44100;
    int n = 4410;

    /* 1. Create/destroy */
    void *wt = wb_wavetable_create(sr);
    if (wt) { printf("  PASS: create\n"); pass++; } else { printf("  FAIL: create\n"); fail++; }
    wb_wavetable_destroy(wt);

    /* 2. Generate sine wavetable, render */
    wt = wb_wavetable_create(sr);
    float *L = (float *)calloc(n, sizeof(float));
    float *R = (float *)calloc(n, sizeof(float));
    wb_wavetable_generate_wavetable(wt, 4, 0); /* SINE preset */
    wb_wavetable_note(wt, 60, 100);
    wb_wavetable_render(wt, L, R, n);
    if (compute_rms(L, n) > 0.001f) { printf("  PASS: sine renders non-silent\n"); pass++; }
    else { printf("  FAIL: sine silent\n"); fail++; }

    /* 3. Generate saw wavetable, render */
    wb_wavetable_generate_wavetable(wt, 4, 1); /* SAW preset */
    wb_wavetable_note(wt, 60, 100);
    wb_wavetable_render(wt, L, R, n);
    if (compute_rms(L, n) > 0.001f) { printf("  PASS: saw renders non-silent\n"); pass++; }
    else { printf("  FAIL: saw silent\n"); fail++; }

    /* 4. Position morphing changes output */
    wb_wavetable_generate_wavetable(wt, 8, 0);
    wb_wavetable_set_position(wt, 0.0f);
    wb_wavetable_note(wt, 60, 100);
    float *L0 = (float *)calloc(n, sizeof(float));
    wb_wavetable_render(wt, L0, R, n);
    wb_wavetable_set_position(wt, 1.0f);
    float *L1 = (float *)calloc(n, sizeof(float));
    wb_wavetable_render(wt, L1, R, n);
    float diff = 0;
    for (int i = 0; i < n; i++) diff += fabsf(L0[i] - L1[i]);
    if (diff > 0.01f) { printf("  PASS: position morphing changes output\n"); pass++; }
    else { printf("  FAIL: position has no effect\n"); fail++; }

    /* 5. Note triggers */
    wb_wavetable_note(wt, 60, 127);
    wb_wavetable_render(wt, L, R, n);
    if (compute_rms(L, n) > 0.001f) { printf("  PASS: note triggers\n"); pass++; }
    else { printf("  FAIL: note silent\n"); fail++; }

    /* 6. Unison produces stereo */
    wb_wavetable_set_unison(wt, 4, 0.5f);
    wb_wavetable_render(wt, L, R, n);
    float l_rms = compute_rms(L, n);
    float r_rms = compute_rms(R, n);
    if (l_rms > 0.001f && r_rms > 0.001f) {
        printf("  PASS: stereo output (L=%.4f R=%.4f)\n", l_rms, r_rms); pass++;
    } else {
        printf("  FAIL: mono output\n"); fail++;
    }

    /* 7. Filter cutoff affects brightness */
    wb_wavetable_set_unison(wt, 1, 0);
    wb_wavetable_set_filter(wt, 2000.0f, 0);
    wb_wavetable_render(wt, L, R, n);
    float rms_bright = compute_rms(L, n);
    wb_wavetable_set_filter(wt, 200.0f, 0);
    wb_wavetable_render(wt, L, R, n);
    float rms_dark = compute_rms(L, n);
    if (rms_bright > rms_dark) { printf("  PASS: filter affects brightness (%.4f > %.4f)\n", rms_bright, rms_dark); pass++; }
    else { printf("  FAIL: filter no effect\n"); fail++; }

    /* 8. Different presets produce different waveforms */
    float *preset0 = (float *)calloc(n, sizeof(float));
    float *preset2 = (float *)calloc(n, sizeof(float));
    wb_wavetable_generate_wavetable(wt, 4, 0); /* sine */
    wb_wavetable_note(wt, 60, 100);
    wb_wavetable_render(wt, preset0, R, n);
    wb_wavetable_generate_wavetable(wt, 4, 2); /* square */
    wb_wavetable_note(wt, 60, 100);
    wb_wavetable_render(wt, preset2, R, n);
    float preset_diff = 0;
    for (int i = 0; i < n; i++) preset_diff += fabsf(preset0[i] - preset2[i]);
    if (preset_diff > 0.01f) { printf("  PASS: different presets differ\n"); pass++; }
    else { printf("  FAIL: presets identical\n"); fail++; }

    /* 9. Interpolation modes work */
    wb_wavetable_generate_wavetable(wt, 4, 0);
    wb_wavetable_note(wt, 60, 100);
    int interp_ok = 1;
    for (int mode = 0; mode <= 2; mode++) {
        wb_wavetable_set_interpolation(wt, mode);
        wb_wavetable_render(wt, L, R, n);
        if (has_nan(L, n) || compute_rms(L, n) < 0.0001f) interp_ok = 0;
    }
    if (interp_ok) { printf("  PASS: all interpolation modes work\n"); pass++; }
    else { printf("  FAIL: interpolation mode broken\n"); fail++; }

    /* 10. Output is finite */
    if (!has_nan(L, n) && !has_nan(R, n)) { printf("  PASS: no NaN/Inf\n"); pass++; }
    else { printf("  FAIL: NaN/Inf detected\n"); fail++; }

    free(L); free(R); free(L0); free(L1); free(preset0); free(preset2);
    wb_wavetable_destroy(wt);

    printf("\nWavetable Synth: %d/%d passed\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
