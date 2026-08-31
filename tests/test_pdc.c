/* tests/test_pdc.c — plugin delay compensation tests. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    printf("PDC tests:\n");

    /* 1. Create PDC with 4 tracks */
    wb_pdc *p = wb_pdc_create(4, 44100);
    CHECK(p != NULL);

    /* 2. Set different latencies per track */
    wb_pdc_set_latency(p, 0, 0);     /* no plugin */
    wb_pdc_set_latency(p, 1, 256);   /* light plugin */
    wb_pdc_set_latency(p, 2, 1024);  /* heavy plugin (max) */
    wb_pdc_set_latency(p, 3, 512);   /* medium plugin */
    CHECK(wb_pdc_get_max_latency(p) == 1024);

    /* 3. Process applies correct delay — verify delay amounts */
    CHECK(wb_pdc_get_delay(p, 0) == 1024);  /* 0 -> delayed to match max */
    CHECK(wb_pdc_get_delay(p, 1) == 768);   /* 256 -> needs 768 more */
    CHECK(wb_pdc_get_delay(p, 2) == 0);     /* max latency -> no delay */
    CHECK(wb_pdc_get_delay(p, 3) == 512);   /* 512 -> needs 512 more */

    /* 4. Track with 0 latency gets delayed to match max — process a ramp and
     * confirm the first 1024 samples of track 0's output are zero (the delay
     * ring buffer is pre-zeroed by calloc). Also confirm track 2 (max latency,
     * delay=0) passes through unchanged. */
    {
        uint32_t frames = 2048;
        wb_sample *buf0 = (wb_sample *)calloc(frames, sizeof(wb_sample));
        wb_sample *buf2 = (wb_sample *)calloc(frames, sizeof(wb_sample));
        /* Provide all 4 tracks to the processor so index 2 == track 2. */
        wb_sample *buf1 = (wb_sample *)calloc(frames, sizeof(wb_sample));
        wb_sample *buf3 = (wb_sample *)calloc(frames, sizeof(wb_sample));
        wb_sample *bufs[4] = { buf0, buf1, buf2, buf3 };

        /* Fill with a ramp so delayed output is distinguishable from zeros. */
        for (uint32_t i = 0; i < frames; i++) {
            buf0[i] = (wb_sample)(i + 1);   /* nonzero everywhere */
            buf1[i] = (wb_sample)(i + 1);
            buf2[i] = (wb_sample)(i + 1);
            buf3[i] = (wb_sample)(i + 1);
        }
        wb_pdc_process(p, bufs, 4, frames);

        /* Track 0 (delay=1024): first 1024 samples should be 0 (ring was zeroed). */
        int track0_delayed = 1;
        for (int i = 0; i < 1024; i++) {
            if (buf0[i] != 0.0f) { track0_delayed = 0; break; }
        }
        CHECK(track0_delayed);

        /* Track 2 (delay=0): output unchanged (no delay applied). */
        int track2_undelayed = 1;
        for (uint32_t i = 0; i < frames; i++) {
            if (buf2[i] != (wb_sample)(i + 1)) { track2_undelayed = 0; break; }
        }
        CHECK(track2_undelayed);

        free(buf0); free(buf1); free(buf2); free(buf3);
    }

    /* 5. Track with max latency stays undelayed — process again with a fresh
     * track 2 buffer and confirm it passes through unchanged. */
    {
        uint32_t frames = 512;
        wb_sample *buf2 = (wb_sample *)calloc(frames, sizeof(wb_sample));
        wb_sample *b0 = (wb_sample *)calloc(frames, sizeof(wb_sample));
        wb_sample *b1 = (wb_sample *)calloc(frames, sizeof(wb_sample));
        for (uint32_t i = 0; i < frames; i++) {
            buf2[i] = (float)(i * 3 + 7);
            b0[i] = (float)(i * 3 + 7);
            b1[i] = (float)(i * 3 + 7);
        }
        /* Process tracks 0,1,2 so index 2 == track 2 (max latency, delay=0). */
        wb_sample *allbufs[3] = { b0, b1, buf2 };
        wb_pdc_process(p, allbufs, 3, frames);
        int undelayed = 1;
        for (uint32_t i = 0; i < frames; i++) {
            if (buf2[i] != (float)(i * 3 + 7)) { undelayed = 0; break; }
        }
        CHECK(undelayed);
        free(b0); free(b1); free(buf2);
    }

    /* 6. Output finite (no NaN) — process a full block and check every sample. */
    {
        uint32_t frames = 4096;
        wb_sample *bufs[4];
        for (int t = 0; t < 4; t++) {
            bufs[t] = (wb_sample *)calloc(frames, sizeof(wb_sample));
            for (uint32_t i = 0; i < frames; i++)
                bufs[t][i] = (wb_sample)sin(2.0 * M_PI * 440.0 * (double)i / 44100.0);
        }
        wb_pdc_process(p, bufs, 4, frames);
        int finite = 1;
        for (int t = 0; t < 4 && finite; t++)
            for (uint32_t i = 0; i < frames; i++)
                if (bufs[t][i] != bufs[t][i]) { finite = 0; break; }
        CHECK(finite);
        for (int t = 0; t < 4; t++) free(bufs[t]);
    }

    /* 7. PDC disable bypasses all delay. */
    {
        wb_pdc_set_enabled(p, 0);
        CHECK(wb_pdc_is_enabled(p) == 0);
        uint32_t frames = 1024;
        wb_sample *buf0 = (wb_sample *)calloc(frames, sizeof(wb_sample));
        for (uint32_t i = 0; i < frames; i++) buf0[i] = (wb_sample)(i + 1);
        wb_sample *bufs[1] = { buf0 };
        wb_pdc_process(p, bufs, 1, frames);
        /* With PDC disabled, output == input even for a track that has delay. */
        int bypassed = 1;
        for (uint32_t i = 0; i < frames; i++) {
            if (buf0[i] != (wb_sample)(i + 1)) { bypassed = 0; break; }
        }
        CHECK(bypassed);
        free(buf0);
        wb_pdc_set_enabled(p, 1);
    }

    /* 8. Latency change recomputes delays. */
    {
        wb_pdc_set_latency(p, 0, 2048);  /* now track 0 is the max */
        CHECK(wb_pdc_get_max_latency(p) == 2048);
        CHECK(wb_pdc_get_delay(p, 0) == 0);
        CHECK(wb_pdc_get_delay(p, 2) == 1024);  /* 1024 -> needs 1024 to match 2048 */
        /* Reset */
        wb_pdc_set_latency(p, 0, 0);
        CHECK(wb_pdc_get_max_latency(p) == 1024);
    }

    wb_pdc_destroy(p);

    printf("\nPDC: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}