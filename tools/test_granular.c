/* test_granular.c — quick smoke test for granular synth */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef float wb_sample;

/* Declarations from wb_granular.c */
void *wb_granular_create(uint32_t sr);
void  wb_granular_destroy(void *inst);
void  wb_granular_load(void *inst, const wb_sample *data, uint32_t count, int loop);
void  wb_granular_note(void *inst, int note, int vel);
void  wb_granular_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

int main(void) {
    void *g = wb_granular_create(44100);
    if (!g) { printf("FAIL: create\n"); return 1; }

    /* Create a 1-second sine wave sample */
    uint32_t count = 44100;
    wb_sample *sample = malloc(count * sizeof(wb_sample));
    for (uint32_t i = 0; i < count; i++) {
        sample[i] = (float)sin((double)i / (double)count * 2.0 * M_PI * 440.0);
    }

    wb_granular_load(g, sample, count, 1);
    wb_granular_note(g, 60, 100);

    wb_sample L[512], R[512];
    memset(L, 0, sizeof(L));
    memset(R, 0, sizeof(R));

    /* Render multiple blocks to accumulate grains */
    float peak = 0;
    for (int block = 0; block < 20; block++) {
        wb_granular_render(g, L, R, 512);
        for (int i = 0; i < 512; i++) {
            float a = fabsf(L[i]);
            if (a > peak) peak = a;
        }
    }

    printf("Granular peak: %.4f\n", peak);
    if (peak > 0.01f) {
        printf("PASS: granular produces audio\n");
    } else {
        printf("FAIL: granular silent\n");
    }

    free(sample);
    wb_granular_destroy(g);
    return (peak > 0.01f) ? 0 : 1;
}
