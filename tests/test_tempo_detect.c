/* tests/test_tempo_detect.c — verify wb_tempo_detect on synthesized click trains.
 * Links only with build/src/wb_tempo_detect.o (no full engine needed). */

#include "wbus.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR 44100
#define MAX_FRAMES (SR * 6)  /* 6 seconds max */

/* Generate a click train: short noise burst every `interval` samples. */
static void gen_click_train(wb_sample *out, uint32_t frames, uint32_t interval,
                            uint32_t click_dur) {
    memset(out, 0, frames * sizeof(wb_sample));
    for (uint32_t pos = 0; pos + click_dur < frames; pos += interval) {
        for (uint32_t i = 0; i < click_dur; i++) {
            /* decaying noise click */
            float r = ((float)(rand() & 0xffff) / 32768.0f) - 0.5f;
            out[pos + i] = r * (1.0f - (float)i / (float)click_dur);
        }
    }
}

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } \
    else { printf("  ok: %s\n", msg); } \
} while (0)

int main(void) {
    wb_sample *buf = malloc(MAX_FRAMES * sizeof(wb_sample));
    if (!buf) { fprintf(stderr, "alloc failed\n"); return 1; }

    /* 1. 120 BPM click train */
    {
        double interval = 60.0 / 120.0 * SR; /* 0.5s = 22050 samples */
        gen_click_train(buf, SR * 4, (uint32_t)(interval + 0.5), 512);
        float bpm = wb_tempo_detect(buf, SR * 4, SR);
        float conf = wb_tempo_detect_confidence();
        printf("120 BPM train -> detected %.1f (conf %.3f)\n", bpm, conf);
        CHECK(fabsf(bpm - 120.0f) <= 5.0f, "120 BPM within ±5");
        CHECK(conf > 0.1f, "120 BPM confidence > 0.1");
    }

    /* 2. 140 BPM click train */
    {
        double interval = 60.0 / 140.0 * SR;
        gen_click_train(buf, SR * 4, (uint32_t)(interval + 0.5), 512);
        float bpm = wb_tempo_detect(buf, SR * 4, SR);
        printf("140 BPM train -> detected %.1f\n", bpm);
        CHECK(fabsf(bpm - 140.0f) <= 5.0f, "140 BPM within ±5");
    }

    /* 3. 90 BPM click train */
    {
        double interval = 60.0 / 90.0 * SR;
        gen_click_train(buf, SR * 5, (uint32_t)(interval + 0.5), 512);
        float bpm = wb_tempo_detect(buf, SR * 5, SR);
        printf("90 BPM train -> detected %.1f\n", bpm);
        CHECK(fabsf(bpm - 90.0f) <= 5.0f, "90 BPM within ±5");
    }

    /* 4. Silence returns 0.0 */
    {
        memset(buf, 0, SR * 2 * sizeof(wb_sample));
        float bpm = wb_tempo_detect(buf, SR * 2, SR);
        printf("silence -> detected %.1f\n", bpm);
        CHECK(bpm == 0.0f, "silence returns 0.0");
    }

    /* 5. Pure tone returns low confidence (no onsets) */
    {
        for (uint32_t i = 0; i < SR * 3; i++)
            buf[i] = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)SR);
        float bpm = wb_tempo_detect(buf, SR * 3, SR);
        float conf = wb_tempo_detect_confidence();
        printf("pure tone -> detected %.1f (conf %.3f)\n", bpm, conf);
        CHECK(conf < 0.3f, "pure tone confidence < 0.3 (low)");
    }

    /* 6. Confidence is in 0..1 range (reuse 120 BPM result bounds check) */
    {
        double interval = 60.0 / 120.0 * SR;
        gen_click_train(buf, SR * 4, (uint32_t)(interval + 0.5), 512);
        wb_tempo_detect(buf, SR * 4, SR);
        float conf = wb_tempo_detect_confidence();
        printf("confidence range check: %.3f\n", conf);
        CHECK(conf >= 0.0f && conf <= 1.0f, "confidence in [0,1]");
    }

    free(buf);
    if (fails) {
        printf("\n%d checks FAILED\n", fails);
        return 1;
    }
    printf("\nALL CHECKS PASSED\n");
    return 0;
}
