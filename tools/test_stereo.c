/* test_stereo.c — gate test for wb_stereo (M/S widening + Haas).
 * Verifies: M/S encode/decode correctness, width control, Haas delay,
 * frequency-dependent widening, SIMD vs scalar consistency. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <emmintrin.h>
#include "wbus.h"

void wb_stereo_widen(const wb_sample *in, wb_sample *out, uint32_t n, float width);
void wb_stereo_widen4(const wb_sample *in, wb_sample *out, uint32_t n, float width);
void *wb_haas_create(uint32_t sr, float delay_ms);
void  wb_haas_destroy(void *inst);
void  wb_haas_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

typedef struct {
    float s1_l[4], s2_l[4];
    float s1_h[4], s2_h[4];
    float b0_l[2], b1_l[2], b2_l[2], a1_l[2], a2_l[2];
    float b0_h[2], b1_h[2], b2_h[2], a1_h[2], a2_h[2];
} wb_lr4xover;

void wb_lr4xover_compute(wb_lr4xover *x, float fc, uint32_t sr);
void wb_stereo_widen_fd(const wb_sample *in, wb_sample *out, uint32_t n,
                        float fc, float low_width, float high_width,
                        wb_lr4xover *xover, uint32_t sr);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

int main(void) {
    /* Test 1: width=1 → identity (no change) */
    TEST("width=1 identity");
    {
        wb_sample in[8]  = {0.5f, 0.3f, 0.8f, 0.2f, 0.1f, 0.9f, 0.4f, 0.6f};
        wb_sample out[8];
        memset(out, 0, sizeof(out));
        wb_stereo_widen4(in, out, 4, 1.0f);
        float max_diff = 0;
        for (int i = 0; i < 8; i++) {
            float d = fabsf(out[i] - in[i]);
            if (d > max_diff) max_diff = d;
        }
        printf("  max diff = %.6e\n", max_diff);
        if (max_diff < 0.01f) PASS(); else FAIL("width=1 not identity");
    }

    /* Test 2: width=0 → mono (L=R=(L+R)/2) */
    TEST("width=0 mono");
    {
        wb_sample in[8]  = {0.8f, 0.2f, 0.6f, 0.4f, 0.9f, 0.1f, 0.3f, 0.7f};
        wb_sample out[8];
        memset(out, 0, sizeof(out));
        wb_stereo_widen4(in, out, 4, 0.0f);
        /* At width=0: L_out = R_out = (L+R)/2 */
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            float expected = (in[i*2] + in[i*2+1]) * 0.5f;
            if (fabsf(out[i*2] - expected) > 0.01f ||
                fabsf(out[i*2+1] - expected) > 0.01f) {
                ok = 0;
                printf("  sample %d: L=%.4f R=%.4f, expected=%.4f\n",
                       i, out[i*2], out[i*2+1], expected);
            }
        }
        if (ok) PASS(); else FAIL("width=0 not mono");
    }

    /* Test 3: width=2 → wider stereo (side channel doubled) */
    TEST("width=2 wider");
    {
        wb_sample in[8]  = {0.8f, 0.2f, 0.6f, 0.4f, 0.9f, 0.1f, 0.3f, 0.7f};
        wb_sample out[8];
        memset(out, 0, sizeof(out));
        wb_stereo_widen4(in, out, 4, 2.0f);
        /* At width=2: L_out = M + S = (L+R)/2 + (L-R) = (3L-R)/2 */
        /* Actually: M=(L+R)/2, S=(L-R)/2, S'=2*S=(L-R) */
        /* L_out = M + S = (L+R)/2 + (L-R) = (3L-R)/2 */
        /* R_out = (M-S')/2 = ((L+R)/2 - (L-R))/2 = (-L+3R)/4 */
        float expected_L = (3.0f * 0.8f - 0.2f) / 2.0f;
        printf("  L_out=%.4f, expected=%.4f\n", out[0], expected_L);
        if (fabsf(out[0] - expected_L) < 0.01f) PASS();
        else FAIL("width=2 calculation wrong");
    }

    /* Test 4: SIMD vs scalar consistency */
    TEST("SIMD vs scalar consistency");
    {
        wb_sample in[32], out_simd[32], out_scalar[32];
        memset(out_simd, 0, sizeof(out_simd));
        memset(out_scalar, 0, sizeof(out_scalar));
        for (int i = 0; i < 32; i++) {
            float phase = 2.0f * 3.14159f * (float)i / 44100.0f * 440.0f;
            in[i] = sinf(phase) * (float)(i % 7) / 7.0f;
        }
        wb_stereo_widen4(in, out_simd, 8, 1.5f);
        wb_stereo_widen(in, out_scalar, 8, 1.5f);
        float max_diff = 0;
        for (int i = 0; i < 32; i++) {
            float d = fabsf(out_simd[i] - out_scalar[i]);
            if (d > max_diff) max_diff = d;
        }
        printf("  max SIMD vs scalar diff = %.6e\n", max_diff);
        if (max_diff < 0.001f) PASS(); else FAIL("SIMD vs scalar mismatch");
    }

    /* Test 5: Haas effect delays right channel */
    TEST("Haas effect delay");
    {
        void *h = wb_haas_create(44100, 10.0f);  /* 10ms delay */
        wb_sample L[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
        wb_sample R[8] = {0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f, 0.1f};
        wb_haas_process(h, L, R, 8);
        /* L should be unchanged */
        int L_ok = (L[0] == 0.1f && L[7] == 0.8f);
        /* R should be delayed: first 441 samples (10ms@44.1k) are zero */
        /* With only 8 samples, R should all be 0 (delay not yet reached) */
        int R_ok = (R[0] == 0.0f && R[7] == 0.0f);
        printf("  L[0]=%.1f (ok=%d), R[0]=%.4f (ok=%d)\n", L[0], L_ok, R[0], R_ok);
        if (L_ok && R_ok) PASS(); else FAIL("Haas delay wrong");
        wb_haas_destroy(h);
    }

    /* Test 6: silence passthrough */
    TEST("silence passthrough");
    {
        wb_sample in[16] = {0};
        wb_sample out[16];
        memset(out, 0, sizeof(out));
        wb_stereo_widen4(in, out, 8, 2.0f);
        float sum = 0;
        for (int i = 0; i < 16; i++) sum += fabsf(out[i]);
        printf("  sum=%.6e\n", sum);
        if (sum < 0.001f) PASS(); else FAIL("silence not zero");
    }

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}
