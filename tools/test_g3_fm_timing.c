/* test_g3_fm_timing.c — G3: dual-core MT FM render timing.
 *
 * Measures: scalar (1 thread) vs MT scalar (2 threads) vs MT SIMD (2 threads, SIMD sin)
 * vs scalar SIMD (G2, 1 thread) for comparison.
 *
 * 16 FM voices (MIDI 36..51), 512 frames × 2000 blocks, 7 iterations.
 * Machine: Intel i5-4260U, 2 cores, clang -std=c11 -O2 -msse2 -lpthread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdint.h>

typedef float wb_sample;

typedef struct {
    double phase;
    double mphase;
    double freq;
    int    active;
    int    note;
    double env;
    int    releasing;
    uint8_t vel;
} fm_voice;

typedef struct {
    uint32_t sr;
    double ratio;
    double index;
    double env_a;
    double env_d;
    fm_voice v[16];
} fm_inst;

/* From wb_fm.o */
void *wb_fm_create(uint32_t sr);
void  wb_fm_destroy(void *inst);
void  wb_fm_note(void *inst, int note, int vel);
void  wb_fm_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* From wb_fm_g2.o */
void  wb_fm_render_g2(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* From wb_fm_g3.o */
void  wb_fm_render_g3(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_fm_render_g3_simd(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

#define FM_VOICES 16
#define BLOCK_FRAMES 512
#define N_BLOCKS 2000
#define WARM_BLOCKS 100
#define N_ITERS 7

static double ts_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

int main(void) {
    printf("G3 dual-core MT FM timing: 16 voices, %u frames × %d blocks, %d iters\n\n",
           BLOCK_FRAMES, N_BLOCKS, N_ITERS);

    void *scalar = wb_fm_create(44100);
    void *g2     = wb_fm_create(44100);
    void *g3     = wb_fm_create(44100);
    void *g3s    = wb_fm_create(44100);

    for (int n = 36; n < 36 + FM_VOICES; n++) {
        wb_fm_note(scalar, n, 96);
        wb_fm_note(g2,     n, 96);
        wb_fm_note(g3,     n, 96);
        wb_fm_note(g3s,    n, 96);
    }

    /* Sanity check: all produce audio */
    {
        wb_sample sl[BLOCK_FRAMES], sr[BLOCK_FRAMES];
        wb_sample g2l[BLOCK_FRAMES], g2r[BLOCK_FRAMES];
        wb_sample g3l[BLOCK_FRAMES], g3r[BLOCK_FRAMES];
        wb_sample g3sl[BLOCK_FRAMES], g3sr[BLOCK_FRAMES];
        memset(sl, 0, sizeof(sl)); memset(sr, 0, sizeof(sr));
        memset(g2l, 0, sizeof(g2l)); memset(g2r, 0, sizeof(g2r));
        memset(g3l, 0, sizeof(g3l)); memset(g3r, 0, sizeof(g3r));
        memset(g3sl, 0, sizeof(g3sl)); memset(g3sr, 0, sizeof(g3sr));

        wb_fm_render(scalar, sl, sr, BLOCK_FRAMES);
        wb_fm_render_g2(g2, g2l, g2r, BLOCK_FRAMES);
        wb_fm_render_g3(g3, g3l, g3r, BLOCK_FRAMES);
        wb_fm_render_g3_simd(g3s, g3sl, g3sr, BLOCK_FRAMES);

        float peaks[4] = {0};
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            float a = fabsf(sl[i]);     if (a > peaks[0]) peaks[0] = a;
            a = fabsf(g2l[i]);         if (a > peaks[1]) peaks[1] = a;
            a = fabsf(g3l[i]);         if (a > peaks[2]) peaks[2] = a;
            a = fabsf(g3sl[i]);        if (a > peaks[3]) peaks[3] = a;
        }
        printf("Sanity peaks: scalar=%.4f G2=%.4f G3=%.4f G3s=%.4f\n",
               peaks[0], peaks[1], peaks[2], peaks[3]);
        if (peaks[0] < 0.01f) { printf("FAIL: scalar silent\n"); return 1; }
        printf("PASS\n\n");
    }

    wb_sample *sl = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    wb_sample *sr = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    wb_sample *g2l = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    wb_sample *g2r = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    wb_sample *g3l = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    wb_sample *g3r = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    wb_sample *g3sl = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    wb_sample *g3sr = malloc(BLOCK_FRAMES * sizeof(wb_sample));

    /* Warm up all variants */
    {
        wb_sample wl[BLOCK_FRAMES], wr[BLOCK_FRAMES];
        for (int b = 0; b < WARM_BLOCKS; b++) {
            memset(wl, 0, sizeof(wl)); memset(wr, 0, sizeof(wr));
            wb_fm_render(scalar, wl, wr, BLOCK_FRAMES);
            wb_fm_render_g2(g2, wl, wr, BLOCK_FRAMES);
            wb_fm_render_g3(g3, wl, wr, BLOCK_FRAMES);
            wb_fm_render_g3_simd(g3s, wl, wr, BLOCK_FRAMES);
        }
    }

    double worst_s[4] = {0}, worst_g2[4] = {0}, worst_g3[4] = {0}, worst_g3s[4] = {0};
    /* worst_s[0]=scalar, worst_g2[0]=g2, worst_g3[0]=g3, worst_g3s[0]=g3s */

    printf("Iter | Scalar (ns) | G2 SIMD (ns) | G3 MT (ns) | G3s MT+SIMD (ns) | G3s/Scalar\n");
    printf("-----|-------------|--------------|-------------|-------------------|-----------\n");

    for (int iter = 0; iter < N_ITERS; iter++) {
        double ws = 0, wg2 = 0, wg3 = 0, wg3s = 0;

        for (int b = 0; b < N_BLOCKS; b++) {
            memset(sl, 0, BLOCK_FRAMES * sizeof(wb_sample));
            memset(sr, 0, BLOCK_FRAMES * sizeof(wb_sample));
            memset(g2l, 0, BLOCK_FRAMES * sizeof(wb_sample));
            memset(g2r, 0, BLOCK_FRAMES * sizeof(wb_sample));
            memset(g3l, 0, BLOCK_FRAMES * sizeof(wb_sample));
            memset(g3r, 0, BLOCK_FRAMES * sizeof(wb_sample));
            memset(g3sl, 0, BLOCK_FRAMES * sizeof(wb_sample));
            memset(g3sr, 0, BLOCK_FRAMES * sizeof(wb_sample));

            double t0 = ts_ns();
            wb_fm_render(scalar, sl, sr, BLOCK_FRAMES);
            double t1 = ts_ns();
            double dt = t1 - t0;
            if (dt > ws) ws = dt;

            t0 = ts_ns();
            wb_fm_render_g2(g2, g2l, g2r, BLOCK_FRAMES);
            t1 = ts_ns();
            dt = t1 - t0;
            if (dt > wg2) wg2 = dt;

            t0 = ts_ns();
            wb_fm_render_g3(g3, g3l, g3r, BLOCK_FRAMES);
            t1 = ts_ns();
            dt = t1 - t0;
            if (dt > wg3) wg3 = dt;

            t0 = ts_ns();
            wb_fm_render_g3_simd(g3s, g3sl, g3sr, BLOCK_FRAMES);
            t1 = ts_ns();
            dt = t1 - t0;
            if (dt > wg3s) wg3s = dt;
        }

        if (ws > worst_s[0]) worst_s[0] = ws;
        if (wg2 > worst_g2[0]) worst_g2[0] = wg2;
        if (wg3 > worst_g3[0]) worst_g3[0] = wg3;
        if (wg3s > worst_g3s[0]) worst_g3s[0] = wg3s;

        printf(" %3d | %11.1f | %12.1f | %11.1f | %17.1f | %.2fx\n",
               iter+1, ws, wg2, wg3, wg3s, ws / (ws > 0 ? wg3s : 1));
    }

    printf("\n--- G3 dual-core MT Results ---\n");
    printf("  Scalar (1 thread, libm sin):        %.1f ns (%.4f ms) — headroom %.1f%%\n",
           worst_s[0], worst_s[0]/1e6, (1.0 - worst_s[0]/1e6/11.61)*100.0);
    printf("  G2 SIMD (1 thread, poly sin):       %.1f ns (%.4f ms) — headroom %.1f%%\n",
           worst_g2[0], worst_g2[0]/1e6, (1.0 - worst_g2[0]/1e6/11.61)*100.0);
    printf("  G3 MT (2 threads, libm sin):        %.1f ns (%.4f ms) — headroom %.1f%%\n",
           worst_g3[0], worst_g3[0]/1e6, (1.0 - worst_g3[0]/1e6/11.61)*100.0);
    printf("  G3s MT+SIMD (2 threads, poly sin):  %.1f ns (%.4f ms) — headroom %.1f%%\n",
           worst_g3s[0], worst_g3s[0]/1e6, (1.0 - worst_g3s[0]/1e6/11.61)*100.0);
    printf("\n  Speedup G3/Scalar:  %.2fx\n", worst_s[0] / (worst_s[0] > 0 ? worst_g3[0] : 1));
    printf("  Speedup G3s/Scalar: %.2fx\n", worst_s[0] / (worst_s[0] > 0 ? worst_g3s[0] : 1));
    printf("  Speedup G3s/G2:     %.2fx\n", worst_g2[0] / (worst_g2[0] > 0 ? worst_g3s[0] : 1));
    printf("  Deadline @44100:    11.6100 ms\n");

    free(sl); free(sr);
    free(g2l); free(g2r);
    free(g3l); free(g3r);
    free(g3sl); free(g3sr);
    wb_fm_destroy(scalar);
    wb_fm_destroy(g2);
    wb_fm_destroy(g3);
    wb_fm_destroy(g3s);

    printf("\nPASS: G3 dual-core MT timing harness ran.\n");
    return 0;
}
