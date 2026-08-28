/* test_fc_timing.c — R076 FC1 measurement harness: wb_osc_render phase-step
 * precompute (linear/exponential envelopes, per-band, disabled-voice skip
 * optimization).
 *
 * Isolates the subtractive synth oscillator render path and measures worst-case
 * per-block wall-clock before vs after replacing the per-sample `phase += inc`
 * with a precomputed per-band `phase_step[fbd]`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "wb_unit.h"
#include "wbus.h"
#include "wb_internal.h"

#define N_NOTES 256
#define BLOCK_FRAMES 512
#define N_BLOCKS 1000
#define WARM_BLOCKS 100

static double ts_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

int main(void) {
    printf("test_fc_timing (R076 FC1: synth osc phase-step precompute)\n");
    printf("  Isolates wb_synth_render_block: %d notes, %d blocks x %u frames\n\n",
           N_NOTES, N_BLOCKS, BLOCK_FRAMES);

    void *sc = wb_synth_create(44100);
    if (!sc) { printf("FAIL: synth create\n"); return 1; }

    /* Activate notes across the synth's voice pool. */
    for (int i = 0; i < N_NOTES; i++) {
        int note = 36 + (i % 36);   /* 36..71 */
        int vel  = 80 + (i % 40);   /* 80..119 */
        wb_synth_note(sc, note, vel);
    }

    /* Sanity: verify audio output */
    {
        wb_sample bufL[BLOCK_FRAMES], bufR[BLOCK_FRAMES];
        memset(bufL, 0, sizeof(bufL));
        memset(bufR, 0, sizeof(bufR));
        wb_synth_render_block(sc, bufL, bufR, BLOCK_FRAMES);
        float peak = 0;
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            float a = fabsf(bufL[i]);
            if (a > peak) peak = a;
            a = fabsf(bufR[i]);
            if (a > peak) peak = a;
        }
        printf("Sanity: %d notes across %d bands produce audio (peak=%.4f, must be >0.01)\n",
               N_BANDS * N_VOICES, N_BANDS, peak);
        if (peak < 0.01f) {
            printf("  FAIL: synth silent — voices not active\n");
            wb_synth_destroy(sc);
            return 1;
        }
        printf("  PASS (peak=%.4f)\n\n", peak);
    }

    /* Warm */
    {
        wb_sample warmL[BLOCK_FRAMES], warmR[BLOCK_FRAMES];
        memset(warmL, 0, sizeof(warmL));
        memset(warmR, 0, sizeof(warmR));
        for (int b = 0; b < WARM_BLOCKS; b++) {
            wb_synth_render_block(sc, warmL, warmR, BLOCK_FRAMES);
        }
    }

    /* Timed run — worst-case + mean per block */
    {
        wb_sample *bufL = malloc(BLOCK_FRAMES * sizeof(wb_sample));
        wb_sample *bufR = malloc(BLOCK_FRAMES * sizeof(wb_sample));
        if (!bufL || !bufR) { printf("FAIL: malloc\n"); return 1; }
        memset(bufL, 0, BLOCK_FRAMES * sizeof(wb_sample));
        memset(bufR, 0, BLOCK_FRAMES * sizeof(wb_sample));

        double worst = 0, sum = 0;
        for (int b = 0; b < N_BLOCKS; b++) {
            double t0 = ts_ns();
            wb_synth_render_block(sc, bufL, bufR, BLOCK_FRAMES);
            double t1 = ts_ns();
            double per = t1 - t0;
            if (per > worst) worst = per;
            sum += per;
            memset(bufL, 0, BLOCK_FRAMES * sizeof(wb_sample));
            memset(bufR, 0, BLOCK_FRAMES * sizeof(wb_sample));
        }

        double mean = sum / N_BLOCKS;
        double period_ms = (BLOCK_FRAMES / (double)44100.0) * 1000.0; /* ~11.61 ms */
        double headroom_pct = (1.0 - (worst / 1e6) / period_ms) * 100.0;

        printf("  worst: %.1f ns  (%.4f ms)\n", worst, worst / 1e6);
        printf("  mean:  %.1f ns  (%.4f ms)\n", mean, mean / 1e6);
        printf("  block period @44100: %.4f ms\n", period_ms);
        printf("  worst-case headroom: %.1f%% of block period\n\n", headroom_pct);

        free(bufL);
        free(bufR);
    }

    wb_synth_destroy(sc);

    printf("PASS: FC1 timing harness ran.\n");
    printf("Note: compare `worst` before vs after osc phase-step precompute to quantify delta.\n");
    return 0;
}
