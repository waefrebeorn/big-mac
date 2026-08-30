/* tests/test_formant.c — headless test of formant shifter */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus.h"

/* Forward declarations */
void *wb_formant_create(uint32_t sr);
void  wb_formant_destroy(void *inst);
void  wb_formant_set_shift(void *inst, float ratio);
void  wb_formant_process(void *inst, float *buf, int n);
void  wb_formant_preset_demon(void *inst);
void  wb_formant_preset_chipmunk(void *inst);

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    uint32_t sr = 44100;
    int n = 4410; /* 100ms */

    printf("=== Formant Shift Test ===\n");

    /* Test 1: Create/destroy */
    void *f = wb_formant_create(sr);
    CHECK(f != NULL);
    wb_formant_destroy(f);

    /* Test 2: Process doesn't crash */
    f = wb_formant_create(sr);
    float *buf = (float *)calloc(n, sizeof(float));
    /* Generate a sine sweep */
    for (int i = 0; i < n; i++)
        buf[i] = sinf(2.0f * M_PI * 440.0f * i / sr);

    wb_formant_set_shift(f, 0.5f);
    wb_formant_process(f, buf, n);

    /* Check output is finite */
    int finite = 1;
    for (int i = 0; i < n; i++) {
        if (!isfinite(buf[i])) { finite = 0; break; }
    }
    CHECK(finite);

    /* Check output is non-zero */
    float sum = 0;
    for (int i = 0; i < n; i++) sum += fabsf(buf[i]);
    CHECK(sum > 0.0f);

    /* Test 3: Different shift ratios produce different output */
    float *buf2 = (float *)calloc(n, sizeof(float));
    memcpy(buf2, buf, n * sizeof(float));
    wb_formant_set_shift(f, 2.0f);
    wb_formant_process(f, buf2, n);

    float diff = 0;
    for (int i = 0; i < n; i++) diff += fabsf(buf[i] - buf2[i]);
    CHECK(diff > 0.0f);

    /* Test 4: Presets don't crash */
    wb_formant_preset_demon(f);
    wb_formant_process(f, buf, n);
    CHECK(isfinite(buf[0]));

    wb_formant_preset_chipmunk(f);
    wb_formant_process(f, buf, n);
    CHECK(isfinite(buf[0]));

    free(buf);
    free(buf2);
    wb_formant_destroy(f);

    printf("\nFormant: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
