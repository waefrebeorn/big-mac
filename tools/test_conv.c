/* test_conv.c — gate test for wb_conv (convolution reverb).
 * Verifies: create/destroy, IR loading, partition structure, impulse response
 * correctness (convolving with a delta should reproduce the IR), dry/wet mix,
 * and output bounds. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "wbus_conv.h"

static int failures = 0;
static int checks = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

/* Generate a simple exponential-decay impulse response. */
static float *make_test_ir(int len, float decay) {
    float *ir = (float *)malloc(len * sizeof(float));
    for (int i = 0; i < len; i++) {
        ir[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * expf(-decay * (float)i / len);
    }
    return ir;
}

/* Generate a delta IR (1.0 at sample 0, 0 elsewhere). */
static float *make_delta_ir(int len) {
    float *ir = (float *)calloc(len, sizeof(float));
    ir[0] = 1.0f;
    return ir;
}

int main(void) {
    printf("=== wb_conv gate ===\n\n");

    /* Test 1: create/destroy */
    printf("Test 1: create/destroy\n");
    wb_conv_inst *c = wb_conv_create(44100);
    CHECK(c != NULL, "create returns non-NULL");
    wb_conv_destroy(c);
    CHECK(1, "destroy doesn't crash");

    /* Test 2: load IR with correct partitioning */
    printf("\nTest 2: load IR (2048 samples, block=128)\n");
    c = wb_conv_create(44100);
    int ir_len = 2048;
    float *ir = make_test_ir(ir_len, 3.0f);
    int rc = wb_conv_load_ir(c, ir, ir_len, 128);
    CHECK(rc == 0, "load_ir returns success");
    int np = wb_conv_get_partitions(c);
    CHECK(np > 0, "has at least 1 partition");
    CHECK(np < 32, "partition count within max");
    printf("  partitions: %d\n", np);
    /* First partition should be block_size */
    CHECK(wb_conv_get_part_len(c, 0) == 128, "first partition = block_size");
    printf("  partition lengths:");
    int total = 0;
    for (int i = 0; i < np; i++) {
        int pl = wb_conv_get_part_len(c, i);
        total += pl;
        printf(" %d", pl);
    }
    printf("\n");
    CHECK(total <= ir_len, "total partition length <= IR length");
    free(ir);

    /* Test 3: delta IR → output should reproduce IR (impulse response test) */
    printf("\nTest 3: delta IR reproduces input (identity convolution)\n");
    {
        int dlen = 512;
        float *delta = make_delta_ir(dlen);
        /* Re-load with delta IR */
        wb_conv_destroy(c);
        c = wb_conv_create(44100);
        rc = wb_conv_load_ir(c, delta, dlen, 128);
        CHECK(rc == 0, "load delta IR");

        /* Feed a single impulse through */
        int block = 128;
        float inL[128] = {0}, inR[128] = {0};
        float outL[128], outR[128];
        inL[0] = 1.0f;
        inR[0] = 1.0f;

        wb_conv_set_mix(c, 1.0f);  /* full wet */
        wb_conv_set_gain(c, 1.0f);
        wb_conv_process(c, inL, inR, outL, outR, block);

        /* With a delta IR and full wet, output[0] should be ~1.0 */
        CHECK(fabsf(outL[0]) > 0.5f, "delta IR: output[0] is significant");
        printf("  outL[0] = %f (expected ~1.0)\n", outL[0]);
        free(delta);
    }

    /* Test 4: dry/wet mix */
    printf("\nTest 4: dry/wet mix\n");
    {
        int dlen = 256;
        float *test_ir = make_test_ir(dlen, 2.0f);
        wb_conv_destroy(c);
        c = wb_conv_create(44100);
        wb_conv_load_ir(c, test_ir, dlen, 128);

        float inL[128], inR[128], outL[128], outR[128];
        for (int i = 0; i < 128; i++) {
            inL[i] = sinf(0.1f * i);
            inR[i] = cosf(0.1f * i);
        }

        /* Full dry: output == input */
        wb_conv_set_mix(c, 0.0f);
        wb_conv_process(c, inL, inR, outL, outR, 128);
        float max_diff = 0;
        for (int i = 0; i < 128; i++) {
            float d = fabsf(outL[i] - inL[i]);
            if (d > max_diff) max_diff = d;
        }
        CHECK(max_diff < 0.001f, "full dry: output equals input");
        printf("  dry max_diff = %e\n", max_diff);

        /* Full wet: output differs from input (reverb tail) */
        wb_conv_set_mix(c, 1.0f);
        wb_conv_process(c, inL, inR, outL, outR, 128);
        float sum_wet = 0;
        for (int i = 0; i < 128; i++) {
            sum_wet += fabsf(outL[i]) + fabsf(outR[i]);
        }
        CHECK(sum_wet > 0.01f, "full wet: output is non-zero");
        printf("  wet sum = %f\n", sum_wet);
        free(test_ir);
    }

    /* Test 5: output stays bounded (no blowup) */
    printf("\nTest 5: output bounds (no blowup)\n");
    {
        int dlen = 1024;
        float *test_ir = make_test_ir(dlen, 1.0f);
        wb_conv_destroy(c);
        c = wb_conv_create(44100);
        wb_conv_load_ir(c, test_ir, dlen, 128);
        wb_conv_set_mix(c, 0.5f);
        wb_conv_set_gain(c, 1.0f);

        float inL[128], inR[128], outL[128], outR[128];
        float max_out = 0;
        /* Run 50 blocks of random-ish input */
        for (int block = 0; block < 50; block++) {
            for (int i = 0; i < 128; i++) {
                inL[i] = sinf(0.05f * (block * 128 + i)) * 0.8f;
                inR[i] = cosf(0.03f * (block * 128 + i)) * 0.8f;
            }
            wb_conv_process(c, inL, inR, outL, outR, 128);
            for (int i = 0; i < 128; i++) {
                float a = fabsf(outL[i]);
                if (a > max_out) max_out = a;
                a = fabsf(outR[i]);
                if (a > max_out) max_out = a;
            }
        }
        CHECK(max_out < 10.0f, "output stays bounded (< 10.0)");
        printf("  max output = %f\n", max_out);
        free(test_ir);
    }

    /* Test 6: silence input → silence output */
    printf("\nTest 6: silence in → silence out\n");
    {
        int dlen = 512;
        float *test_ir = make_test_ir(dlen, 2.0f);
        wb_conv_destroy(c);
        c = wb_conv_create(44100);
        wb_conv_load_ir(c, test_ir, dlen, 128);
        wb_conv_set_mix(c, 1.0f);

        float inL[128] = {0}, inR[128] = {0};
        float outL[128], outR[128];
        wb_conv_process(c, inL, inR, outL, outR, 128);
        float sum = 0;
        for (int i = 0; i < 128; i++) {
            sum += fabsf(outL[i]) + fabsf(outR[i]);
        }
        CHECK(sum < 0.001f, "silence input → silence output");
        printf("  silence sum = %e\n", sum);
        free(test_ir);
    }

    wb_conv_destroy(c);

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures > 0 ? 1 : 0;
}
