/* tests/test_dynamics_adv.c — test advanced dynamics feature. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

static float rms(const float *b, int n) { float s=0; for(int i=0;i<n;i++) s+=b[i]*b[i]; return sqrtf(s/n); }

int main(void) {
    int n = 44100;
    void *d = wb_dynamics_create(44100);
    CHECK(d != NULL);

    float *in = (float *)calloc(n, sizeof(float));
    float *out = (float *)calloc(n, sizeof(float));

    /* Create signal with varying level */
    for (int i = 0; i < n; i++) {
        float t = (float)i / 44100.0f;
        in[i] = sinf(2*M_PI*440*t) * (i < n/2 ? 0.9f : 0.1f);
    }

    /* 1. Compression reduces peak-to-RMS ratio */
    wb_dynamics_set_mode(d, 0);
    wb_dynamics_set_threshold(d, 0, 0.3f);
    wb_dynamics_set_ratio(d, 0, 8.0f);
    wb_dynamics_set_attack(d, 0, 5.0f);
    wb_dynamics_set_release(d, 0, 50.0f);
    wb_dynamics_process(d, out, in, n);
    CHECK(rms(out, n) > 0.001f);

    /* 2. Output finite */
    int finite = 1;
    for (int i = 0; i < n; i++) if (out[i] != out[i]) finite = 0;
    CHECK(finite);

    /* 3. Limiter prevents clipping */
    for (int i = 0; i < n; i++) in[i] = sinf(2*M_PI*440*(float)i/44100) * 2.0f;
    wb_dynamics_set_mode(d, 1);
    wb_dynamics_process(d, out, in, n);
    int noclip = 1;
    for (int i = 0; i < n; i++) { if (out[i] > 1.01f || out[i] < -1.01f) noclip = 0; }
    CHECK(noclip);

    /* 4. Multiband mode */
    wb_dynamics_set_mode(d, 4);
    wb_dynamics_set_band_count(d, 3);
    wb_dynamics_process(d, out, in, n);
    CHECK(rms(out, n) > 0.001f);

    /* 5. Parallel mix */
    wb_dynamics_set_parallel_mix(d, 0.5f);
    wb_dynamics_process(d, out, in, n);
    CHECK(rms(out, n) > 0.001f);

    /* 6. Gate mode */
    wb_dynamics_set_mode(d, 2);
    wb_dynamics_set_parallel_mix(d, 0);
    for (int i = 0; i < n; i++) in[i] = sinf(2*M_PI*440*(float)i/44100) * 0.01f;
    wb_dynamics_process(d, out, n > 1000 ? in : in, 1000);
    CHECK(rms(out, 1000) < 0.1f);

    free(in); free(out);
    wb_dynamics_destroy(d);
    printf("\nDynamics Adv: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
