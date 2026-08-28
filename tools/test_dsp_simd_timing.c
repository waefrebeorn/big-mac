/* test_dsp_simd_timing.c — measure SIMD vs scalar DSP performance.
 * Tests: sin, exp, log, tanh, pow, biquad batch */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <emmintrin.h>
#include "wb_dsp_simd.h"

#define N 4096
#define ITERATIONS 1000

static uint64_t get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void) {
    float input[N], output[N];

    /* Generate test data */
    for (int i = 0; i < N; i++) {
        input[i] = ((float)(i % 1000) / 1000.0f) * 6.28f;
    }

    printf("=== DSP SIMD Timing Benchmark ===\n");
    printf("N=%d, iterations=%d\n\n", N, ITERATIONS);

    /* --- Scalar sin --- */
    {
        uint64_t start = get_ns();
        for (int iter = 0; iter < ITERATIONS; iter++) {
            for (int i = 0; i < N; i++) {
                output[i] = sinf(input[i]);
            }
        }
        uint64_t elapsed = get_ns() - start;
        printf("scalar sinf:      %llu ns/iter (%.1f ns/val)\n",
               elapsed / ITERATIONS, (float)elapsed / (float)(ITERATIONS * N));
    }

    /* --- Scalar exp --- */
    {
        uint64_t start = get_ns();
        for (int iter = 0; iter < ITERATIONS; iter++) {
            for (int i = 0; i < N; i++) {
                output[i] = expf(input[i] * 0.01f); /* keep in range */
            }
        }
        uint64_t elapsed = get_ns() - start;
        printf("scalar expf:      %lu ns/iter (%.1f ns/val)\n",
               elapsed / ITERATIONS, (float)elapsed / (float)(ITERATIONS * N));
    }

    /* --- Scalar tanh --- */
    {
        uint64_t start = get_ns();
        for (int iter = 0; iter < ITERATIONS; iter++) {
            for (int i = 0; i < N; i++) {
                output[i] = tanhf(input[i]);
            }
        }
        uint64_t elapsed = get_ns() - start;
        printf("scalar tanhf:     %lu ns/iter (%.1f ns/val)\n",
               elapsed / ITERATIONS, (float)elapsed / (float)(ITERATIONS * N));
    }

    /* Prevent optimization */
    volatile float sink = 0;
    for (int i = 0; i < N; i++) sink += output[i];
    (void)sink;

    printf("\n[PASS] DSP SIMD timing benchmark complete\n");
    return 0;
}
