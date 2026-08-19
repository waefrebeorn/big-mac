/* test_loudness_meter.c — R018-G live gated loudness meter verification.
 * Feeds synthetic audio through wb_loudness_meter and asserts the streaming
 * integrated/short-term LUFS are finite, sane, and track signal level. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus/wbus_voice_polish.h"

static int failures = 0, checks = 0;
#define CK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== R018-G live loudness meter ===\n\n");
    const int sr = 44100, ch = 2;
    const uint32_t block = (uint32_t)sr; /* 1 s */

    /* Build a 2 s tone at -20 dBFS-ish amplitude and a quiet one. */
    float *loud = malloc((size_t)block * 2 * ch * sizeof(float));
    float *quiet = malloc((size_t)block * 2 * ch * sizeof(float));
    for (uint32_t i = 0; i < block * 2 * (uint32_t)ch; i++) {
        double s = sin(2.0 * 3.14159 * 220.0 * i / sr);
        loud[i]  = (float)(0.20 * s);   /* ~ -14 dBFS peak */
        quiet[i] = (float)(0.02 * s);   /* ~ -34 dBFS peak */
    }

    wb_loudness_meter *m = wb_loudness_meter_create((float)sr);
    CK(m != NULL, "meter created");

    /* insufficient data before a full 400 ms block */
    wb_loudness_meter_process(m, loud, 100, ch);
    CK(wb_loudness_meter_integrated(m) <= -60.0f, "integrated undefined before 400ms (returns -inf-ish)");

    /* process full 2 s */
    wb_loudness_meter_process(m, loud, block, ch);
    wb_loudness_meter_process(m, loud, block, ch);
    float loud_int = wb_loudness_meter_integrated(m);
    float loud_st  = wb_loudness_meter_short_term(m);
    printf("  [dbg] loud integrated=%.2f short-term=%.2f LUFS\n", loud_int, loud_st);
    CK(loud_int > -40.0f && loud_int < -5.0f, "loud integrated LUFS in sane range");
    CK(loud_st  > -40.0f && loud_st  < -5.0f, "short-term LUFS in sane range");
    CK(loud_int > -1e8f, "integrated is finite (not -inf)");

    /* quiet signal must measure lower LUFS than loud */
    wb_loudness_meter_reset(m);
    wb_loudness_meter_process(m, quiet, block, ch);
    wb_loudness_meter_process(m, quiet, block, ch);
    float quiet_int = wb_loudness_meter_integrated(m);
    printf("  [dbg] quiet integrated=%.2f LUFS\n", quiet_int);
    CK(quiet_int < loud_int - 3.0f, "quieter signal measures lower LUFS (meter tracks level)");

    wb_loudness_meter_destroy(m);
    free(loud); free(quiet);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
