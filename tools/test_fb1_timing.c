/* test_fb1_timing.c — R076 FB1 measurement harness.
 *
 * Isolates the FM render path (wb_fm_render) and measures worst-case
 * per-block wall-clock before vs after hoisting the envelope exp()
 * constants (aD, aA) out of the per-sample loop.
 *
 * Setup: FM instance, 16 voices active (all MIDI notes 36..51),
 * render 1000 blocks of 512 samples, report worst-case per-block ns.
 *
 * Before FB1: 2*16*512 = 16384 exp() calls per block.
 * After FB1:  2 exp() calls per block (hoisted outside the loop).
 *
 * Command: ./build/wb_test_fb1
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
    printf("test_fb1_timing\n");
    printf("  Isolates wb_fm_render: 16 active voices, %d blocks x %u frames\n\n",
           N_BLOCKS, BLOCK_FRAMES);

    void *fm = wb_fm_create(44100);
    if (!fm) { printf("FAIL: fm create\n"); return 1; }

    /* activate all 16 voices with different notes */
    for (int note = 36; note < 36 + N_VOICES; note++) {
        wb_fm_note(fm, note, 100);
    }

    /* verify all 16 voices are active */
    {
        /* fm_inst internals are opaque; verify by rendering a block
         * and checking non-silent output */
        wb_sample buf[BLOCK_FRAMES * 2];
        memset(buf, 0, sizeof(buf));
        wb_fm_render(fm, buf, buf + BLOCK_FRAMES, BLOCK_FRAMES);
        float peak = 0;
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            float a = fabs(buf[i]);
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

    /* warm */
    {
        wb_sample warm[BLOCK_FRAMES * 2];
        memset(warm, 0, sizeof(warm));
        for (int b = 0; b < WARM_BLOCKS; b++) {
            wb_fm_render(fm, warm, warm + BLOCK_FRAMES, BLOCK_FRAMES);
        }
    }

    /* timed run */
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
        /* reset buffers for next block (FM render overwrites, but be safe) */
        memset(bufL, 0, BLOCK_FRAMES * sizeof(wb_sample));
        memset(bufR, 0, BLOCK_FRAMES * sizeof(wb_sample));
    }

    double mean = sum / (double)N_BLOCKS;
    double block_period_ms = (double)BLOCK_FRAMES / 44100.0 * 1000.0;
    double worst_ms = worst * 1e-6;

    printf("FM render timing (worst-case per %u-frame block):\n", BLOCK_FRAMES);
    printf("  worst: %.1f ns  (%.4f ms)\n", worst, worst_ms);
    printf("  mean:  %.1f ns  (%.4f ms)\n", mean, mean * 1e-6);
    printf("  block period @44100: %.4f ms\n", block_period_ms);
    printf("  worst-case headroom: %.1f%% of block period\n\n",
           100.0 * (1.0 - worst_ms / block_period_ms));

    /* exp() call count analysis (worst-case: all 16 voices active entire block) */
    {
        double exp_per_block_before = 2.0 * (double)N_VOICES * (double)BLOCK_FRAMES;
        double exp_per_block_after  = 2.0;
        double exp_saved = exp_per_block_before - exp_per_block_after;
        printf("exp() call count analysis (16 voices, 512 frames):\n");
        printf("  before FB1: %.0f exp() calls per block (2 * %d voices * %u frames)\n",
               exp_per_block_before, N_VOICES, BLOCK_FRAMES);
        printf("  after FB1:  %.0f exp() calls per block (hoisted)\n",
               exp_per_block_after);
        printf("  saved:      %.0f exp() calls per block\n\n", exp_saved);
    }

    free(bufL);
    free(bufR);
    wb_fm_destroy(fm);

    printf("PASS: FB1 timing harness ran.\n");
    printf("Note: compare this output before vs after the exp() hoist to quantify the delta.\n");
    return 0;
}
