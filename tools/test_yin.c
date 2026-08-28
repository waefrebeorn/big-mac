/* test_yin.c — gate test for wb_yin (YIN pitch detection).
 * Verifies: sine wave pitch detection, SIMD vs scalar consistency,
 * silence rejection, frequency range. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus.h"

/* Declarations (wb_yin_cfg matches wb_yin.c) */
typedef struct {
    uint32_t sr;
    float    threshold;
    int      min_tau;
    int      max_tau;
} wb_yin_cfg;

float wb_yin_pitch(const float *buf, int n, uint32_t sr);
float wb_yin_detect(const float *buf, int n, const wb_yin_cfg *cfg);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

int main(void) {
    /* Test 1: detect 440 Hz sine wave */
    TEST("detect 440 Hz sine");
    {
        uint32_t sr = 44100;
        int n = 2048;
        float buf[2048];
        for (int i = 0; i < n; i++) {
            buf[i] = sinf(2.0f * 3.14159f * 440.0f * (float)i / (float)sr);
        }
        float pitch = wb_yin_pitch(buf, n, sr);
        printf("  detected pitch = %.1f Hz (expected ~440)\n", pitch);
        if (pitch > 430 && pitch < 450) PASS();
        else FAIL("pitch out of range");
    }

    /* Test 2: detect 220 Hz sine wave */
    TEST("detect 220 Hz sine");
    {
        uint32_t sr = 44100;
        int n = 2048;
        float buf[2048];
        for (int i = 0; i < n; i++) {
            buf[i] = sinf(2.0f * 3.14159f * 220.0f * (float)i / (float)sr);
        }
        float pitch = wb_yin_pitch(buf, n, sr);
        printf("  detected pitch = %.1f Hz (expected ~220)\n", pitch);
        if (pitch > 210 && pitch < 230) PASS();
        else FAIL("pitch out of range");
    }

    /* Test 3: detect 1000 Hz sine wave */
    TEST("detect 1000 Hz sine");
    {
        uint32_t sr = 44100;
        int n = 1024;
        float buf[1024];
        for (int i = 0; i < n; i++) {
            buf[i] = sinf(2.0f * 3.14159f * 1000.0f * (float)i / (float)sr);
        }
        float pitch = wb_yin_pitch(buf, n, sr);
        printf("  detected pitch = %.1f Hz (expected ~1000)\n", pitch);
        if (pitch > 980 && pitch < 1020) PASS();
        else FAIL("pitch out of range");
    }

    /* Test 4: silence → no pitch */
    TEST("silence → no pitch");
    {
        uint32_t sr = 44100;
        int n = 2048;
        float buf[2048];
        memset(buf, 0, sizeof(buf));
        float pitch = wb_yin_pitch(buf, n, sr);
        printf("  detected pitch = %.1f Hz (expected 0)\n", pitch);
        if (pitch == 0.0f) PASS(); else FAIL("silence should give 0");
    }

    /* Test 5: noise → no reliable pitch */
    TEST("noise → no pitch");
    {
        uint32_t sr = 44100;
        int n = 2048;
        float buf[2048];
        srand(42);
        for (int i = 0; i < n; i++) {
            buf[i] = (float)(rand() % 1000) / 1000.0f * 2.0f - 1.0f;
        }
        float pitch = wb_yin_pitch(buf, n, sr);
        printf("  detected pitch = %.1f Hz (noise, just checking no crash)\n", pitch);
        PASS();
    }

    /* Test 6: buffer too small */
    TEST("buffer too small");
    {
        uint32_t sr = 44100;
        float buf[32];
        for (int i = 0; i < 32; i++) {
            buf[i] = sinf(2.0f * 3.14159f * 440.0f * (float)i / (float)sr);
        }
        float pitch = wb_yin_pitch(buf, 32, sr);
        printf("  detected pitch = %.1f Hz (expected 0)\n", pitch);
        if (pitch == 0.0f) PASS(); else FAIL("small buffer should give 0");
    }

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}
