/* tests/test_mpe.c — MPE (MIDI Polyphonic Expression) tests. */
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

/* Compute spectral centroid proxy: ratio of high-freq energy to total.
 * Simple approach: difference between consecutive samples as "high-freq content". */
static float spectral_diff(const float *buf, int n) {
    float sum = 0;
    for (int i = 1; i < n; i++) sum += fabsf(buf[i] - buf[i-1]);
    return sum / (float)(n - 1);
}

int main(void) {
    int pass = 0, fail = 0;
    uint32_t sr = 44100;
    int n = 4410; /* 0.1s */

    /* 1. Create/destroy */
    void *mpe = wb_mpe_create(sr);
    if (mpe) { printf("  PASS: create\n"); pass++; }
    else { printf("  FAIL: create\n"); fail++; return 1; }
    wb_mpe_destroy(mpe);

    /* 2. Note on increases active count */
    mpe = wb_mpe_create(sr);
    wb_mpe_note_on(mpe, 0, 60, 100);
    if (wb_mpe_active_notes(mpe) == 1) { printf("  PASS: note on -> active count = 1\n"); pass++; }
    else { printf("  FAIL: note on active count = %d\n", wb_mpe_active_notes(mpe)); fail++; }

    /* 3. Note off decreases active count */
    wb_mpe_note_off(mpe, 0, 60);
    if (wb_mpe_active_notes(mpe) == 0) { printf("  PASS: note off -> active count = 0\n"); pass++; }
    else { printf("  FAIL: note off active count = %d\n", wb_mpe_active_notes(mpe)); fail++; }

    /* 4. Render produces non-silent audio */
    wb_mpe_note_on(mpe, 0, 60, 100);
    float *out = (float *)calloc(n, sizeof(float));
    wb_mpe_render(mpe, out, n);
    float rms = compute_rms(out, n);
    if (rms > 0.001f) { printf("  PASS: render non-silent (rms=%.4f)\n", rms); pass++; }
    else { printf("  FAIL: render silent (rms=%.6f)\n", rms); fail++; }

    /* 5. Pitch bend changes frequency (phase advance rate) */
    /* Render without bend, then with +12 semitones bend. Higher pitch = faster zero crossings. */
    float *out_no_bend = (float *)calloc(n, sizeof(float));
    /* Re-trigger to reset phase */
    wb_mpe_note_off(mpe, 0, 60);
    wb_mpe_note_on(mpe, 0, 60, 100);
    wb_mpe_set_pitch_bend(mpe, 0, 60, 0.0f);
    wb_mpe_render(mpe, out_no_bend, n);
    float zc_no = 0;
    for (int i = 1; i < n; i++)
        if ((out_no_bend[i] >= 0) != (out_no_bend[i-1] >= 0)) zc_no++;

    float *out_bend = (float *)calloc(n, sizeof(float));
    wb_mpe_note_off(mpe, 0, 60);
    wb_mpe_note_on(mpe, 0, 60, 100);
    wb_mpe_set_pitch_bend(mpe, 0, 60, 12.0f); /* +1 octave */
    wb_mpe_render(mpe, out_bend, n);
    float zc_bend = 0;
    for (int i = 1; i < n; i++)
        if ((out_bend[i] >= 0) != (out_bend[i-1] >= 0)) zc_bend++;

    if (zc_bend > zc_no * 1.5f) {
        printf("  PASS: pitch bend increases freq (zc: %.0f -> %.0f)\n", zc_no, zc_bend); pass++;
    } else {
        printf("  FAIL: pitch bend no freq change (zc: %.0f -> %.0f)\n", zc_no, zc_bend); fail++;
    }

    /* 6. Pressure affects amplitude */
    wb_mpe_note_off(mpe, 0, 60);
    wb_mpe_note_on(mpe, 0, 60, 100);
    wb_mpe_set_pitch_bend(mpe, 0, 60, 0.0f);

    wb_mpe_set_pressure(mpe, 0, 60, 1.0f);
    float *out_full = (float *)calloc(n, sizeof(float));
    wb_mpe_render(mpe, out_full, n);
    float rms_full = compute_rms(out_full, n);

    wb_mpe_note_off(mpe, 0, 60);
    wb_mpe_note_on(mpe, 0, 60, 100);
    wb_mpe_set_pitch_bend(mpe, 0, 60, 0.0f);
    wb_mpe_set_pressure(mpe, 0, 60, 0.2f);
    float *out_quiet = (float *)calloc(n, sizeof(float));
    wb_mpe_render(mpe, out_quiet, n);
    float rms_quiet = compute_rms(out_quiet, n);

    if (rms_quiet < rms_full * 0.7f) {
        printf("  PASS: pressure affects amplitude (full=%.4f quiet=%.4f)\n", rms_full, rms_quiet); pass++;
    } else {
        printf("  FAIL: pressure no amplitude change (full=%.4f quiet=%.4f)\n", rms_full, rms_quiet); fail++;
    }

    /* 7. Timbre changes spectral content (filter cutoff) */
    wb_mpe_note_off(mpe, 0, 60);
    wb_mpe_note_on(mpe, 0, 60, 100);
    wb_mpe_set_pitch_bend(mpe, 0, 60, 0.0f);
    wb_mpe_set_pressure(mpe, 0, 60, 1.0f);

    wb_mpe_set_timbre(mpe, 0, 60, 0.0f); /* dark */
    float *out_dark = (float *)calloc(n, sizeof(float));
    wb_mpe_render(mpe, out_dark, n);
    float diff_dark = spectral_diff(out_dark, n);

    wb_mpe_note_off(mpe, 0, 60);
    wb_mpe_note_on(mpe, 0, 60, 100);
    wb_mpe_set_pitch_bend(mpe, 0, 60, 0.0f);
    wb_mpe_set_pressure(mpe, 0, 60, 1.0f);
    wb_mpe_set_timbre(mpe, 0, 60, 1.0f); /* bright */
    float *out_bright = (float *)calloc(n, sizeof(float));
    wb_mpe_render(mpe, out_bright, n);
    float diff_bright = spectral_diff(out_bright, n);

    if (diff_bright > diff_dark * 1.2f) {
        printf("  PASS: timbre changes spectrum (dark=%.4f bright=%.4f)\n", diff_dark, diff_bright); pass++;
    } else {
        printf("  FAIL: timbre no spectral change (dark=%.4f bright=%.4f)\n", diff_dark, diff_bright); fail++;
    }

    /* 8. Global bend affects all notes */
    wb_mpe_note_off(mpe, 0, 60);
    wb_mpe_note_on(mpe, 0, 60, 100);
    wb_mpe_note_on(mpe, 1, 64, 100);
    wb_mpe_set_pitch_bend(mpe, 0, 60, 0.0f);
    wb_mpe_set_pitch_bend(mpe, 1, 64, 0.0f);
    wb_mpe_set_pressure(mpe, 0, 60, 1.0f);
    wb_mpe_set_pressure(mpe, 1, 64, 1.0f);
    wb_mpe_set_global_bend(mpe, 0.0f);
    float *out_g0 = (float *)calloc(n, sizeof(float));
    wb_mpe_render(mpe, out_g0, n);
    float zc_g0 = 0;
    for (int i = 1; i < n; i++)
        if ((out_g0[i] >= 0) != (out_g0[i-1] >= 0)) zc_g0++;

    wb_mpe_note_off(mpe, 0, 60);
    wb_mpe_note_off(mpe, 1, 64);
    wb_mpe_note_on(mpe, 0, 60, 100);
    wb_mpe_note_on(mpe, 1, 64, 100);
    wb_mpe_set_pitch_bend(mpe, 0, 60, 0.0f);
    wb_mpe_set_pitch_bend(mpe, 1, 64, 0.0f);
    wb_mpe_set_pressure(mpe, 0, 60, 1.0f);
    wb_mpe_set_pressure(mpe, 1, 64, 1.0f);
    wb_mpe_set_global_bend(mpe, 7.0f); /* +perfect fifth */
    float *out_g7 = (float *)calloc(n, sizeof(float));
    wb_mpe_render(mpe, out_g7, n);
    float zc_g7 = 0;
    for (int i = 1; i < n; i++)
        if ((out_g7[i] >= 0) != (out_g7[i-1] >= 0)) zc_g7++;

    if (zc_g7 > zc_g0 * 1.2f) {
        printf("  PASS: global bend affects all notes (zc: %.0f -> %.0f)\n", zc_g0, zc_g7); pass++;
    } else {
        printf("  FAIL: global bend no effect (zc: %.0f -> %.0f)\n", zc_g0, zc_g7); fail++;
    }

    /* 9. Max 16 voices */
    wb_mpe_destroy(mpe);
    mpe = wb_mpe_create(sr);
    for (int i = 0; i < 20; i++) {
        wb_mpe_note_on(mpe, i % 16, 48 + (i % 12), 100);
    }
    int active = wb_mpe_active_notes(mpe);
    if (active <= 16) { printf("  PASS: max 16 voices (active=%d)\n", active); pass++; }
    else { printf("  FAIL: exceeded 16 voices (active=%d)\n", active); fail++; }

    wb_mpe_destroy(mpe);
    free(out); free(out_no_bend); free(out_bend);
    free(out_full); free(out_quiet);
    free(out_dark); free(out_bright);
    free(out_g0); free(out_g7);

    printf("\nMPE: %d passed, %d failed\n", pass, fail);
    return fail > 0 ? 1 : 0;
}