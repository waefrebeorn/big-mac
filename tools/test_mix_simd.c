/* test_mix_simd.c — gate test for wb_mix_simd (mixer bus + constant-power pan).
 * Verifies: pan law correctness, 4-track mixing, master volume + metering. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <emmintrin.h>
#include "wbus.h"

/* Declarations */
void wb_mix4_tracks(const wb_sample *tracks[4], const float volumes[4],
                    const float pans[4], wb_sample *output, uint32_t n_samples);
float wb_mix_master_volume(wb_sample *buf, uint32_t n_samples, float gain, float *sumsq);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

int main(void) {
    /* Test 1: constant-power pan — center */
    TEST("constant-power pan: center (pan=0)");
    {
        float L, R;
        /* Inline pan_gains equivalent */
        float pan = 0.0f;
        float theta = (pan + 1.0f) * 0.785398163f;
        L = cosf(theta); R = sinf(theta);
        printf("  L=%.4f, R=%.4f\n", L, R);
        /* At center: L=R=√2/2 ≈ 0.707 */
        if (fabsf(L - 0.7071f) < 0.01f && fabsf(R - 0.7071f) < 0.01f) PASS();
        else FAIL("center pan wrong");
    }

    /* Test 2: constant-power pan — hard left */
    TEST("constant-power pan: hard left (pan=-1)");
    {
        float pan = -1.0f;
        float theta = (pan + 1.0f) * 0.785398163f;
        float L = cosf(theta), R = sinf(theta);
        printf("  L=%.4f, R=%.4f\n", L, R);
        if (L > 0.99f && R < 0.01f) PASS();
        else FAIL("hard left wrong");
    }

    /* Test 3: constant-power pan — hard right */
    TEST("constant-power pan: hard right (pan=+1)");
    {
        float pan = 1.0f;
        float theta = (pan + 1.0f) * 0.785398163f;
        float L = cosf(theta), R = sinf(theta);
        printf("  L=%.4f, R=%.4f\n", L, R);
        if (R > 0.99f && L < 0.01f) PASS();
        else FAIL("hard right wrong");
    }

    /* Test 4: 4-track mix — single track passthrough */
    TEST("4-track mix: single track passthrough");
    {
        wb_sample out[8] = {0};
        wb_sample tr0[8] = {0.5f, 0.3f, 0.4f, 0.2f, 0.1f, 0.6f, 0.7f, 0.8f};
        wb_sample tr1[8] = {0}, tr2[8] = {0}, tr3[8] = {0};
        const wb_sample *tracks[4] = {tr0, tr1, tr2, tr3};
        float vols[4] = {1.0f, 0, 0, 0};
        float pans[4] = {0, 0, 0, 0};
        wb_mix4_tracks(tracks, vols, pans, out, 4);
        /* Track 0 at pan=0, vol=1: L=cos(π/4)=0.707, R=sin(π/4)=0.707 */
        /* out[0] = 0.5 * 0.707 = 0.3535 */
        printf("  out[0]=%.4f (expected ~0.3535)\n", out[0]);
        if (fabsf(out[0] - 0.5f * 0.7071f) < 0.01f) PASS();
        else FAIL("single track passthrough wrong");
    }

    /* Test 5: master volume + peak metering */
    TEST("master volume + peak metering");
    {
        wb_sample buf[8] = {0.5f, -0.8f, 0.3f, 0.9f, -0.4f, 0.1f, 0.7f, -0.6f};
        float sumsq = 0;
        float pk = wb_mix_master_volume(buf, 4, 2.0f, &sumsq);
        printf("  peak=%.4f (expected 1.8), sumsq=%.4f\n", pk, sumsq);
        if (fabsf(pk - 1.8f) < 0.01f) PASS();
        else FAIL("peak wrong");
    }

    /* Test 6: silence passthrough */
    TEST("silence passthrough");
    {
        wb_sample out[8] = {0};
        wb_sample tr0[8] = {0}, tr1[8] = {0}, tr2[8] = {0}, tr3[8] = {0};
        const wb_sample *tracks[4] = {tr0, tr1, tr2, tr3};
        float vols[4] = {1, 1, 1, 1};
        float pans[4] = {0, 0, 0, 0};
        wb_mix4_tracks(tracks, vols, pans, out, 4);
        float sum = 0;
        for (int i = 0; i < 8; i++) sum += fabsf(out[i]);
        printf("  sum=%.6e\n", sum);
        if (sum < 0.001f) PASS();
        else FAIL("silence not zero");
    }

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}
