/* test_fm_timing.c — R076 FM render timing harness.
 *
 * Compares inline phase stepping vs precomputed phase_step[]/mphase_step[].
 *
 * Setup: FM instance, 16 voices active (MIDI notes 36..51), render 1000
 * blocks of 512 samples, report worst-case + mean per-block ns.
 *
 * The harness links against wb_fm.o directly (from build/src/), so the
 * wb_fm_render it calls is whatever is currently compiled in the repo.
 *
 * For R076 FB1+FB2 comparison, we build two variants:
 *   before: inline phase stepping  (no FB2, FB1 already applied)
 *   after:  phase_step[k] + mphase_step[k] (FB2 applied, FB1 already applied)
 *
 * Before FB2:  v->mphase += TWO_PI * v->freq * f->ratio / sr;  (per sample)
 * After FB2:   v->mphase += mphase_step[k];                     (per voice, once)
 *
 * 16 voices * 512 frames = 8192 samples. Each sample step recalculates the
 * TWO_PI * freq / sr division+multiply. That's 8192×2 = 16384 redundant
 * divide+multiply ops per block, per voice-path (carrier + modulator).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "wb_unit.h"
#include "wbus.h"
#include "wb_internal.h"

#define N_VOICES 16
#define BLOCK_FRAMES 512
#define N_BLOCKS 1000
#define WARM_BLOCKS 100

static double ts_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

int main(void) {
    printf("test_fm_timing (R076 FB2: phase-step precompute)\n");
    printf("  Isolates wb_fm_render: 16 active voices, %d blocks x %u frames\n",
           N_BLOCKS, BLOCK_FRAMES);

    void *fm = wb_fm_create(44100);
    if (!fm) { printf("FAIL: fm create\n"); return 1; }

    /* Activate all 16 voices with different notes (C2..C3 range) */
    for (int note = 36; note < 36 + N_VOICES; note++) {
        wb_fm_note(fm, note, 100);
    }

    /* Sanity: verify all 16 voices produce audio */
    {
        wb_sample buf[BLOCK_FRAMES * 2];
        memset(buf, 0, sizeof(buf));
        wb_fm_render(fm, buf, buf + BLOCK_FRAMES, BLOCK_FRAMES);
        float peak = 0;
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            float a = fabsf(buf[i]);
            if (a > peak) peak = a;
        }
        printf("Sanity: 16 active FM voices produce audio (peak=%.4f, must be >0.01)\n",
               peak);
        if (peak < 0.01f) {
            printf("  FAIL: FM render silent — voices not active\n");
            wb_fm_destroy(fm);
            return 1;
        }
        printf("  PASS (peak=%.4f)\n\n", peak);
    }

    /* Warm up */
    {
        wb_sample warm[BLOCK_FRAMES * 2];
        memset(warm, 0, sizeof(warm));
        for (int b = 0; b < WARM_BLOCKS; b++) {
            wb_fm_render(fm, warm, warm + BLOCK_FRAMES, BLOCK_FRAMES);
        }
    }

    /* Timed run — worst-case + mean per block */
    wb_sample *bufL = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    wb_sample *bufR = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    if (!bufL || !bufR) { printf("FAIL: malloc\n"); return 1; }
    memset(bufL, 0, BLOCK_FRAMES * sizeof(wb_sample));
    memset(bufR, 0, BLOCK_FRAMES * sizeof(wb_sample));

    double worst = 0, sum = 0;
    for (int b = 0; b < N_BLOCKS; b++) {
        double t0 = ts_ns();
        wb_fm_render(fm, bufL, bufR, BLOCK_FRAMES);
        double t1 = ts_ns();
        double per = t1 - t0;
        if (per > worst) worst = per;
        sum += per;
        /* Reset buffers for next block (FM render overwrites L/R) */
        memset(bufL, 0, BLOCK_FRAMES * sizeof(wb_sample));
        memset(bufR, 0, BLOCK_FRAMES * sizeof(wb_sample));
    }

    double mean = sum / N_BLOCKS;
    double period_ms = (BLOCK_FRAMES / (double)44100.0) * 1000.0; /* 11.6099... ms */
    double headroom_pct = (1.0 - (worst / 1e6) / period_ms) * 100.0;

    printf("  worst: %.1f ns  (%.4f ms)\n", worst, worst / 1e6);
    printf("  mean:  %.1f ns  (%.4f ms)\n", mean, mean / 1e6);
    printf("  block period @44100: %.4f ms\n", period_ms);
    printf("  worst-case headroom: %.1f%% of block period\n\n", headroom_pct);

    /* Cleanup */
    free(bufL);
    free(bufR);
    wb_fm_destroy(fm);

    printf("PASS: FM timing harness ran.\n");
    printf("Note: compare `worst` before vs after phase-step precompute to quantify delta.\n");
    return 0;
}
