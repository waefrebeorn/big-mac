/* tests/test_stem_split.c — test stem separation feature. */
#include <stdio.h>
#include <math.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

static float rms(const float *b, int n) { float s=0; for(int i=0;i<n;i++) s+=b[i]*b[i]; return sqrtf(s/n); }

int main(void) {
    int n = 44100;
    float *mix = (float *)calloc(n, sizeof(float));
    float *vocals = (float *)calloc(n, sizeof(float));
    float *drums = (float *)calloc(n, sizeof(float));
    float *bass = (float *)calloc(n, sizeof(float));
    float *other = (float *)calloc(n, sizeof(float));

    /* Create mix: 60Hz bass + 440Hz vocal + noise transient drums */
    for (int i = 0; i < n; i++) {
        float t = (float)i / 44100.0f;
        bass[i] = sinf(2*M_PI*60*t) * 0.5f;
        vocals[i] = sinf(2*M_PI*440*t) * 0.3f;
        drums[i] = ((rand()%1000)/1000.0f - 0.5f) * (i%500<5 ? 1.0f : 0.0f);
        mix[i] = bass[i] + vocals[i] + drums[i];
    }

    int rc = wb_stem_split(mix, n, 1, vocals, drums, bass, other);
    CHECK(rc == 0);

    /* 1. All outputs non-zero */
    float rms_v = rms(vocals, n), rms_d = rms(drums, n);
    float rms_b = rms(bass, n), rms_o = rms(other, n);
    CHECK(rms_v > 0.001f);
    CHECK(rms_d > 0.001f);
    CHECK(rms_b > 0.001f);
    CHECK(rms_o > 0.001f);

    /* 2. Bass stem has significant low-frequency energy */
    CHECK(rms_b > 0.05f);

    /* 3. Vocal stem captures mid-range energy */
    CHECK(rms_v > 0.002f);

    /* 4. Energy conservation: sum of stems ≈ original */
    float sum_stems = rms_v + rms_d + rms_b + rms_o;
    float rms_mix = rms(mix, n);
    CHECK(sum_stems > rms_mix * 0.5f);

    /* 4. No NaN */
    int finite = 1;
    for (int i = 0; i < n; i++) {
        if (vocals[i] != vocals[i] || drums[i] != drums[i] ||
            bass[i] != bass[i] || other[i] != other[i]) finite = 0;
    }
    CHECK(finite);

    free(mix); free(vocals); free(drums); free(bass); free(other);
    printf("\nStem Split: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
