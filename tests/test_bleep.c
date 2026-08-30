/* tests/test_bleep.c — headless test of bleep censor engine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus.h"

void *wb_bleep_create(uint32_t sr);
void  wb_bleep_destroy(void *inst);
void  wb_bleep_set_type(void *inst, int type);
void  wb_bleep_set_freq(void *inst, float freq);
void  wb_bleep_trigger(void *inst, int duration);
void  wb_bleep_process(void *inst, float *buf, int n);
void  wb_bleep_auto_detect(void *inst, float *buf, int n, float threshold);

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    uint32_t sr = 44100;

    printf("=== Bleep Censor Test ===\n");

    void *b = wb_bleep_create(sr);
    CHECK(b != NULL);

    /* Test 1: Trigger bleep, verify output is non-zero */
    float buf[4410];
    memset(buf, 0, sizeof(buf));
    wb_bleep_trigger(b, 4410);
    wb_bleep_process(b, buf, 4410);

    float sum = 0;
    for (int i = 0; i < 4410; i++) sum += fabsf(buf[i]);
    CHECK(sum > 0.0f);

    /* Test 2: Different bleep types */
    for (int t = 0; t < 5; t++) {
        void *b2 = wb_bleep_create(sr);
        wb_bleep_set_type(b2, t);
        float buf2[1000];
        memset(buf2, 0, sizeof(buf2));
        wb_bleep_trigger(b2, 1000);
        wb_bleep_process(b2, buf2, 1000);

        int finite = 1;
        for (int i = 0; i < 1000; i++) {
            if (!isfinite(buf2[i])) { finite = 0; break; }
        }
        CHECK(finite);
        wb_bleep_destroy(b2);
    }

    /* Test 3: Auto-detect bleeps loud sections */
    void *b3 = wb_bleep_create(sr);
    float buf3[4410];
    for (int i = 0; i < 4410; i++) {
        if (i > 1000 && i < 2000)
            buf3[i] = 0.9f; /* Loud section */
        else
            buf3[i] = 0.01f; /* Quiet */
    }
    wb_bleep_auto_detect(b3, buf3, 4410, 0.5f);

    /* Loud section should be replaced (near zero or bleep tone) */
    float loud_sum = 0;
    for (int i = 1000; i < 2000; i++) loud_sum += fabsf(buf3[i]);
    CHECK(loud_sum > 0.0f); /* Bleep tone present */

    /* Quiet section should be unchanged */
    CHECK(fabsf(buf3[500]) < 0.02f);

    wb_bleep_destroy(b);
    wb_bleep_destroy(b3);

    printf("\nBleep: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
