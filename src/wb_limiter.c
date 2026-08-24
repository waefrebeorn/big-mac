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

/* ---- R073 hop 39: streaming (delay-line) variant for the RT path ---------- */
struct wb_stream_limiter {
    float    *delay;
    uint32_t  cap;
    uint32_t  w;
    float     ceiling;
    float     release_a;
    float     gain;
};

wb_stream_limiter *wb_stream_limiter_create(double sr, double lookahead_ms,
                                            float ceiling) {
    wb_stream_limiter *lm = calloc(1, sizeof(*lm));
    if (!lm) return NULL;
    lm->cap = (uint32_t)(lookahead_ms * sr / 1000.0);
    if (lm->cap < 8) lm->cap = 8;
    lm->delay = calloc((size_t)lm->cap * 2, sizeof(wb_sample));
    if (!lm->delay) { free(lm); return NULL; }
    lm->ceiling = ceiling > 0 ? ceiling : 1.0f;
    lm->release_a = (float)exp(-1.0 / (0.010 * sr));
    lm->gain = 1.0f;
    return lm;
}

void wb_stream_limiter_destroy(wb_stream_limiter *lm) {
    if (!lm) return;
    free(lm->delay);
    free(lm);
}

/* processes exactly one interleaved stereo frame */
void wb_stream_limiter_frame(wb_stream_limiter *lm, wb_sample *frame) {
    /* 1. compute needed gain from max(|raw incoming|, ring content);
     *    the ring holds the NEXT `cap` frames to be output (lookahead) */
    float peak = fabsf(frame[0]) > fabsf(frame[1])
               ? fabsf(frame[0]) : fabsf(frame[1]);
    for (uint32_t k = 0; k < lm->cap; k++) {
        float a = fabsf(lm->delay[k*2]);
        float b = fabsf(lm->delay[k*2+1]);
        if (a > peak) peak = a;
        if (b > peak) peak = b;
    }
    float needed = peak > lm->ceiling ? lm->ceiling / peak : 1.0f;

    /* 2. envelope: instant attack on needed < gain; smooth release up */
    if (needed < lm->gain) lm->gain = needed;
    else lm->gain = needed + (lm->gain - needed) * lm->release_a;

    /* 3. output the DELAYED sample at the current gain, store the RAW
     *    incoming for its turn `cap` frames from now */
    uint32_t ridx = lm->w % lm->cap;
    float out0 = lm->delay[ridx*2]   * lm->gain;
    float out1 = lm->delay[ridx*2+1] * lm->gain;
    /* store raw incoming into the slot just vacated */
    lm->delay[ridx*2]   = frame[0];
    lm->delay[ridx*2+1] = frame[1];
    frame[0] = (wb_sample)out0;
    frame[1] = (wb_sample)out1;
    lm->w++;
}

float wb_stream_limiter_gain(const wb_stream_limiter *lm) {
    return lm ? lm->gain : 0.0f;
}
