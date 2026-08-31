/* tests/test_vocal_synth.c — test vocal/formant synthesis feature. */
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
    int n = 4410; /* 100ms */

    /* 1. Create/destroy */
    void *v = wb_vocal_synth_create(sr);
    if (v) { printf("  PASS: create\n"); pass++; } else { printf("  FAIL: create\n"); fail++; }
    wb_vocal_synth_destroy(v);

    /* 2. Set pitch, render — non-silent */
    v = wb_vocal_synth_create(sr);
    float *buf = (float *)calloc(n, sizeof(float));
    wb_vocal_synth_set_pitch(v, 60);
    wb_vocal_synth_render(v, buf, n);
    float rms = compute_rms(buf, n);
    if (rms > 0.001f) { printf("  PASS: non-silent output (rms=%.4f)\n", rms); pass++; }
    else { printf("  FAIL: silent output (rms=%.4f)\n", rms); fail++; }

    /* 3. Different vowels produce different output */
    float *buf_ah = (float *)calloc(n, sizeof(float));
    wb_vocal_synth_set_vowel(v, 0.0f);
    wb_vocal_synth_render(v, buf_ah, n);
    float *buf_ee = (float *)calloc(n, sizeof(float));
    wb_vocal_synth_set_vowel(v, 0.5f);
    wb_vocal_synth_render(v, buf_ee, n);
    float diff = 0;
    for (int i = 0; i < n; i++) diff += fabsf(buf_ah[i] - buf_ee[i]);
    if (diff > 0.01f) { printf("  PASS: different vowels differ\n"); pass++; }
    else { printf("  FAIL: vowels identical\n"); fail++; }

    /* 4. Breathiness adds noise */
    wb_vocal_synth_set_vowel(v, 0.0f);
    wb_vocal_synth_set_breathiness(v, 0.0f);
    float *buf_dry = (float *)calloc(n, sizeof(float));
    wb_vocal_synth_render(v, buf_dry, n);
    wb_vocal_synth_set_breathiness(v, 0.8f);
    float *buf_wet = (float *)calloc(n, sizeof(float));
    wb_vocal_synth_render(v, buf_wet, n);
    float rms_dry = compute_rms(buf_dry, n);
    float rms_wet = compute_rms(buf_wet, n);
    if (rms_wet > rms_dry) { printf("  PASS: breathiness increases level (%.4f > %.4f)\n", rms_wet, rms_dry); pass++; }
    else { printf("  FAIL: breathiness no effect\n"); fail++; }

    /* 5. Phoneme string */
    wb_vocal_synth_speak(v, "ah eh ee");
    float *buf_spell = (float *)calloc(n, sizeof(float));
    wb_vocal_synth_render(v, buf_spell, n);
    if (compute_rms(buf_spell, n) > 0.001f) { printf("  PASS: speak produces output\n"); pass++; }
    else { printf("  FAIL: speak silent\n"); fail++; }

    /* 6. No NaN */
    if (!has_nan(buf, n)) { printf("  PASS: no NaN\n"); pass++; }
    else { printf("  FAIL: NaN detected\n"); fail++; }

    /* 7. No clipping */
    float peak = 0;
    for (int i = 0; i < n; i++) {
        float a = fabsf(buf[i]);
        if (a > peak) peak = a;
    }
    if (peak < 1.5f) { printf("  PASS: no clipping (peak=%.3f)\n", peak); pass++; }
    else { printf("  FAIL: clipping (peak=%.3f)\n", peak); fail++; }

    /* 8. Different pitches */
    wb_vocal_synth_set_pitch(v, 48);
    float *buf_low = (float *)calloc(n, sizeof(float));
    wb_vocal_synth_render(v, buf_low, n);
    wb_vocal_synth_set_pitch(v, 72);
    float *buf_high = (float *)calloc(n, sizeof(float));
    wb_vocal_synth_render(v, buf_high, n);
    /* High pitch should have more high-frequency content — compare zero crossings */
    int zc_low = 0, zc_high = 0;
    for (int i = 1; i < n; i++) {
        if ((buf_low[i] > 0) != (buf_low[i-1] > 0)) zc_low++;
        if ((buf_high[i] > 0) != (buf_high[i-1] > 0)) zc_high++;
    }
    if (zc_high > zc_low) { printf("  PASS: higher pitch has more zero crossings (%d > %d)\n", zc_high, zc_low); pass++; }
    else { printf("  FAIL: pitch effect on ZC (%d vs %d)\n", zc_high, zc_low); fail++; }

    /* 9. Render with no phonemes (inactive) produces silence */
    void *v2 = wb_vocal_synth_create(sr);
    float *buf_silence = (float *)calloc(n, sizeof(float));
    wb_vocal_synth_render(v2, buf_silence, n);
    if (compute_rms(buf_silence, n) < 0.0001f) { printf("  PASS: inactive = silence\n"); pass++; }
    else { printf("  FAIL: inactive not silent\n"); fail++; }
    wb_vocal_synth_destroy(v2);

    free(buf); free(buf_ah); free(buf_ee); free(buf_dry); free(buf_wet);
    free(buf_spell); free(buf_low); free(buf_high); free(buf_silence);
    wb_vocal_synth_destroy(v);

    printf("\nVocal Synth: %d/%d passed\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
