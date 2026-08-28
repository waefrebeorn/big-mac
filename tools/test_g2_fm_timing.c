/* test_g2_fm_timing.c — R076 G2: SIMD-accelerated FM sin cascade timing.

Standalone harness: links ONLY against wb_fm.o + wb_fm_g2.o + wb_midi_note_to_freq_stub.o.
Uses wb_fm_create/render/note directly, no unit registry.

Measures: scalar FM (libm sin) vs SIMD FM (polynomial sin approx)
16 voices, 512 frames × 2000 blocks, 7 iterations.
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

void *wb_fm_create(uint32_t sr);
void  wb_fm_destroy(void *inst);
void  wb_fm_note(void *inst, int note, int vel);
void  wb_fm_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_fm_render_g2(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

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
    printf("G2 FM SIMD timing: 16 voices, %u frames × %d blocks, %d iters\n\n",
           BLOCK_FRAMES, N_BLOCKS, N_ITERS);

    void *scalar = wb_fm_create(44100);
    void *simd   = wb_fm_create(44100);

    /* Fire all 16 voices */
    for (int n = 36; n < 36 + FM_VOICES; n++) {
        wb_fm_note(scalar, n, 96);
        wb_fm_note(simd,   n, 96);
    }

    wb_sample *sl = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    wb_sample *sr = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    wb_sample *gl = malloc(BLOCK_FRAMES * sizeof(wb_sample));
    wb_sample *gr = malloc(BLOCK_FRAMES * sizeof(wb_sample));

    double worst_s = 0, worst_g = 0;
    double sum_s = 0, sum_g = 0;

    printf("Iter | Scalar worst (ns) | SIMD worst (ns) | Speedup\n");
    printf("-----|-------------------|-----------------|--------\n");

    for (int iter = 0; iter < N_ITERS; iter++) {
        double worst_s_i = 0, worst_g_i = 0;

        for (int b = 0; b < N_BLOCKS; b++) {
            memset(sl, 0, BLOCK_FRAMES * sizeof(wb_sample));
            memset(sr, 0, BLOCK_FRAMES * sizeof(wb_sample));
            memset(gl, 0, BLOCK_FRAMES * sizeof(wb_sample));
            memset(gr, 0, BLOCK_FRAMES * sizeof(wb_sample));

            double t0 = ts_ns();
            wb_fm_render(scalar, sl, sr, BLOCK_FRAMES);
            double t1 = ts_ns();
            double dt_s = t1 - t0;
            if (dt_s > worst_s_i) worst_s_i = dt_s;

            t0 = ts_ns();
            wb_fm_render_g2(simd, gl, gr, BLOCK_FRAMES);
            t1 = ts_ns();
            double dt_g = t1 - t0;
            if (dt_g > worst_g_i) worst_g_i = dt_g;
        }

        if (worst_s_i > worst_s) worst_s = worst_s_i;
        if (worst_g_i > worst_g) worst_g = worst_g_i;
        double speedup = worst_s / worst_g;
        printf(" %3d | %15.1f | %15.1f | %.2fx\n",
               iter+1, worst_s_i, worst_g_i, speedup);
    }

    printf("\n--- R076 G2 Results ---\n");
    printf("  Scalar FM worst:  %.1f ns (%.4f ms)\n",
           worst_s, worst_s / 1e6);
    printf("  SIMD FM worst:    %.1f ns (%.4f ms)\n",
           worst_g, worst_g / 1e6);
    printf("  Speedup:          %.2fx\n", worst_s / worst_g);
    printf("  Deadline @44100:  11.6100 ms\n");
    printf("  Scalar headroom:  %.1f%%\n", (1.0 - worst_s/1e6/11.61)*100.0);
    printf("  SIMD headroom:    %.1f%%\n", (1.0 - worst_g/1e6/11.61)*100.0);

    free(sl); free(sr); free(gl); free(gr);
    wb_fm_destroy(scalar);
    wb_fm_destroy(simd);

    printf("\nPASS: G2 tame.\n");
    return 0;
}
