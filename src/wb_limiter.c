/* wb_limiter.c — R073 hop 37: lookahead brickwall limiter (two-pass).
 *
 * Pass 1 computes per-frame required gain from a sliding forward max over the
 * lookahead window; pass 2 applies the smoothed envelope (instant attack,
 * ~10ms release). Offline-friendly. Pure arithmetic; C11, no third party.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_limiter.h"

struct wb_limiter {
    float  ceiling;
    float  release_a;
    uint32_t la_frames;
};

wb_limiter *wb_limiter_create(double sr, double lookahead_ms, float ceiling) {
    wb_limiter *lm = calloc(1, sizeof(*lm));
    if (!lm) return NULL;
    lm->la_frames = (uint32_t)(lookahead_ms * sr / 1000.0);
    if (lm->la_frames < 8) lm->la_frames = 8;
    lm->ceiling = ceiling > 0 ? ceiling : 1.0f;
    lm->release_a = (float)exp(-1.0 / (0.010 * sr));
    return lm;
}

void wb_limiter_destroy(wb_limiter *lm) { free(lm); }

int wb_limiter_process(wb_limiter *lm, wb_sample *buf, uint32_t frames) {
    if (!lm || !buf || frames == 0) return -1;
    float *gain = malloc((size_t)frames * sizeof(float));
    if (!gain) return -1;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t end = i + lm->la_frames;
        if (end > frames) end = frames;
        float peak = 0;
        for (uint32_t j = i; j < end; j++) {
            float a = fabsf(buf[j*2]);
            float b = fabsf(buf[j*2+1]);
            float m = a > b ? a : b;
            if (m > peak) peak = m;
        }
        gain[i] = peak > lm->ceiling ? lm->ceiling / peak : 1.0f;
    }
    float g = 1.0f;
    for (uint32_t i = 0; i < frames; i++) {
        if (gain[i] < g) g = gain[i];
        else             g = gain[i] + (g - gain[i]) * lm->release_a;
        gain[i] = g;
    }
    for (uint32_t i = 0; i < frames; i++) {
        buf[i*2]   = (wb_sample)(buf[i*2]   * gain[i]);
        buf[i*2+1] = (wb_sample)(buf[i*2+1] * gain[i]);
    }
    free(gain);
    return 0;
}
