/* tests/test_wah.c — headless test of auto-wah / envelope filter */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus.h"

void *wb_wah_create(uint32_t sr);
void  wb_wah_destroy(void *inst);
void  wb_wah_set_mode(void *inst, int mode);
void  wb_wah_set_range(void *inst, float min, float max);
void  wb_wah_set_q(void *inst, float q);
void  wb_wah_set_pedal(void *inst, float pos);
void  wb_wah_set_lfo_rate(void *inst, float rate);
void  wb_wah_process(void *inst, float *buf, int n);
void  wb_wah_preset_crybaby(void *inst);
void  wb_wah_preset_funk(void *inst);
void  wb_wah_preset_talking(void *inst);

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    uint32_t sr = 44100;
    int n = 4410; /* 100ms */
    printf("=== Auto-Wah Test ===\n");

    void *w = wb_wah_create(sr);
    CHECK(w != NULL);

    /* Generate test signal: sine sweep with amplitude envelope */
    float *buf = (float *)malloc(n * sizeof(float));
    for (int i = 0; i < n; i++) {
        float env = (i < n / 2) ? 0.8f : 0.1f; /* Loud then quiet */
        buf[i] = env * sinf(2.0f * M_PI * 440.0f * i / sr);
    }

    /* Test 1: Auto mode — envelope should sweep filter */
    wb_wah_set_mode(w, 0); /* WAH_AUTO */
    wb_wah_set_range(w, 300.0f, 3000.0f);
    wb_wah_set_q(w, 8.0f);
    wb_wah_process(w, buf, n);

    /* Output should be finite */
    int finite = 1;
    for (int i = 0; i < n; i++) {
        if (!isfinite(buf[i])) { finite = 0; break; }
    }
    CHECK(finite);

    /* Output should be non-zero */
    float sum = 0;
    for (int i = 0; i < n; i++) sum += fabsf(buf[i]);
    CHECK(sum > 0.0f);

    /* Test 2: LFO mode */
    void *w2 = wb_wah_create(sr);
    wb_wah_set_mode(w2, 1); /* WAH_LFO */
    wb_wah_set_lfo_rate(w2, 5.0f);
    float *buf2 = (float *)malloc(n * sizeof(float));
    for (int i = 0; i < n; i++)
        buf2[i] = 0.5f * sinf(2.0f * M_PI * 440.0f * i / sr);
    wb_wah_process(w2, buf2, n);
    float sum2 = 0;
    for (int i = 0; i < n; i++) sum2 += fabsf(buf2[i]);
    CHECK(sum2 > 0.0f);
    free(buf2);
    wb_wah_destroy(w2);

    /* Test 3: Pedal mode */
    void *w3 = wb_wah_create(sr);
    wb_wah_set_mode(w3, 2); /* WAH_PEDAL */
    wb_wah_set_pedal(w3, 0.7f);
    float *buf3 = (float *)malloc(n * sizeof(float));
    for (int i = 0; i < n; i++)
        buf3[i] = 0.5f * sinf(2.0f * M_PI * 440.0f * i / sr);
    wb_wah_process(w3, buf3, n);
    float sum3 = 0;
    for (int i = 0; i < n; i++) sum3 += fabsf(buf3[i]);
    CHECK(sum3 > 0.0f);
    free(buf3);
    wb_wah_destroy(w3);

    /* Test 4: Talking mode */
    void *w4 = wb_wah_create(sr);
    wb_wah_set_mode(w4, 3); /* WAH_TALKING */
    float *buf4 = (float *)malloc(n * sizeof(float));
    for (int i = 0; i < n; i++)
        buf4[i] = sinf(2.0f * M_PI * 440.0f * i / sr);
    wb_wah_process(w4, buf4, n);
    float sum4 = 0;
    for (int i = 0; i < n; i++) sum4 += fabsf(buf4[i]);
    CHECK(sum4 > 0.0f);
    free(buf4);
    wb_wah_destroy(w4);

    /* Test 5: Presets */
    wb_wah_preset_crybaby(w);
    wb_wah_process(w, buf, n);
    CHECK(isfinite(buf[0]));

    wb_wah_preset_funk(w);
    wb_wah_process(w, buf, n);
    CHECK(isfinite(buf[0]));

    wb_wah_preset_talking(w);
    wb_wah_process(w, buf, n);
    CHECK(isfinite(buf[0]));

    free(buf);
    wb_wah_destroy(w);

    printf("\nWah: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
