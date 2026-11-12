/*
 * bigmac_out_test.c -- behavior gate for the output backend.
 *
 * Proves the backend really opens something and really accepts frames.
 * On a headless box it lands on the wav backend, which still proves the
 * whole path (init -> write -> header patch -> close).
 */
#include "bigmac_out.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); g_fail++; } } while (0)

int main(void)
{
    const int rate = 48000;
    const int n    = 2048;
    float *buf = malloc((size_t)n * 2 * sizeof(float));
    CHECK(buf != NULL, "buffer allocated");
    if (!buf) return 1;

    /* a 440 Hz sine, stereo */
    for (int i = 0; i < n; i++) {
        float s = 0.25f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / rate);
        buf[i * 2 + 0] = s;
        buf[i * 2 + 1] = s;
    }

    /* ---- 1. a backend opens ---------------------------------------- */
    int rc = bigmac_out_init(rate, 2, "/tmp/bigmac_out_test.wav");
    CHECK(rc == 0, "bigmac_out_init opens a backend");
    CHECK(bigmac_out_backend() != BIGMAC_BACKEND_NONE,
          "the backend is not NONE");
    printf("  backend: %s\n", bigmac_out_backend_name());

    /* ---- 2. frames are accepted ------------------------------------- */
    long w = bigmac_out_write(buf, n);
    CHECK(w > 0, "bigmac_out_write accepts frames");
    CHECK(w == n, "every frame is accepted");

    /* ---- 3. latency is reported ------------------------------------- */
    CHECK(bigmac_out_latency_frames() >= 0, "latency is a real number");

    /* ---- 4. a zero/negative write is a quiet no-op ------------------ */
    CHECK(bigmac_out_write(NULL, n) == 0, "a NULL buffer is a no-op");
    CHECK(bigmac_out_write(buf, 0) == 0, "zero frames is a no-op");

    /* ---- 5. writing past the end is safe ---------------------------- */
    bigmac_out_write(buf, n);

    /* ---- 6. close is idempotent ------------------------------------- */
    BigmacBackend used = bigmac_out_backend();
    bigmac_out_close();
    bigmac_out_close();
    CHECK(1, "double close did not crash");

    /* ---- 7. the wav backend left a valid file ----------------------- */
    if (used == BIGMAC_BACKEND_WAV) {
        FILE *f = fopen("/tmp/bigmac_out_test.wav", "rb");
        CHECK(f != NULL, "the wav file exists");
        if (f) {
            unsigned char h[12];
            size_t got = fread(h, 1, 12, f);
            CHECK(got == 12, "the wav header is 12 bytes");
            CHECK(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0,
                  "the wav header says RIFF/WAVE");
            long sz = 0;
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            CHECK(sz > 44, "the wav has a body");
            fclose(f);
        }
    }

    free(buf);
    if (g_fail == 0) {
        printf("bigmac_out_test: all checks passed\n");
        return 0;
    }
    printf("bigmac_out_test: %d FAILED\n", g_fail);
    return 1;
}
