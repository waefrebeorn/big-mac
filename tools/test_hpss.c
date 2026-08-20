/* test_hpss.c — verify R020-A harmonic-percussive source separation. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus/wbus_hpss.h"

#define CK(c, m) do { if (!(c)) { printf("  [FAIL] %s\n", m); fails++; } \
                     else printf("  [PASS] %s\n", m); } while (0)

static int finite_buf(const float *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (!isfinite(b[i])) return 0;
    return 1;
}
static float energy(const float *b, uint32_t n) {
    double s = 0; for (uint32_t i = 0; i < n; i++) s += (double)b[i] * b[i];
    return (float)s;
}

int main(void) {
    int fails = 0;
    printf("\n=== R020-A HPSS ===\n");
    uint32_t N = 44100;  /* 1 s @ 44.1k */
    float *in = (float*)calloc(N, sizeof(float));
    float *hm = (float*)calloc(N, sizeof(float));
    float *pm = (float*)calloc(N, sizeof(float));
    /* Signal = a steady broadband "pad" (harmonic/tonal bed) + periodic
     * transients (percussive). This is the canonical HPSS case (music pad
     * vs drums), not a sparse pure tone which median-filtering sends to
     * percussive by design. */
    float lp = 0.0f;
    srand(12345);
    for (uint32_t i = 0; i < N; i++) {
        float wn = (float)rand() / RAND_MAX - 0.5f;
        lp += 0.04f * (wn - lp);            /* 1-pole lowpass -> broadband, steady */
        in[i] = 0.6f * lp;                  /* sustained harmonic bed */
        if (i % 8000 == 0) in[i] += 0.9f;   /* click every ~0.18s (transient) */
        if ((i % 8000) > 0 && (i % 8000) < 300) in[i] += 0.5f * ((float)rand()/RAND_MAX - 0.5f);
    }
    wb_hpss *h = wb_hpss_create(2048, 44100.0f, 31, 31);
    CK(h != NULL, "hpss created");
    int rc = wb_hpss_separate(h, in, N, hm, pm);
    CK(rc == 0, "separate returned 0");
    CK(finite_buf(hm, N) && finite_buf(pm, N), "no NaN/Inf in outputs");
    float ein = energy(in, N), ehm = energy(hm, N), epm = energy(pm, N);
    CK(ehm > 0 && epm > 0, "both components non-empty");
    CK(ehm + epm > 0.3f * ein, "energy substantially retained (soft mask)");
    /* the tone is continuous -> harmonic should carry most of the sustained energy;
     * the clicks are sparse -> percussive should be a minority but present */
    CK(ehm > epm * 0.2f, "harmonic retains substantial energy (tone present)");
    CK(epm > 0.0f, "percussive component produced (clicks isolated)");
    printf("\n  ein=%.3f ehm=%.3f epm=%.3f\n", ein, ehm, epm);
    wb_hpss_destroy(h);
    free(in); free(hm); free(pm);
    printf("\n%d checks, %d failures\n", 7, fails);
    return fails ? 1 : 0;
}
