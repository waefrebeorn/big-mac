/* wb_surround.c — surround sound panning (5.1/7.1).
 *
 * R078: Professional surround mixing.
 *
 * Speaker layouts:
 *   5.1: L, C, R, Ls, Rs, LFE
 *   7.1: L, C, R, Lss, Rss, Lsr, Rsr, LFE
 *
 * Panning: position in 2D space -> gain vector per speaker
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define SURROUND_MAX_SPEAKERS 8

typedef enum {
    SURROUND_STEREO = 0,
    SURROUND_5_1,
    SURROUND_7_1
} surround_format_t;

typedef struct {
    float x, y;      /* Speaker position (-1..1) */
    float gain;      /* Current gain */
} speaker_t;

typedef struct {
    uint32_t sr;
    surround_format_t format;
    int      num_speakers;
    speaker_t speakers[SURROUND_MAX_SPEAKERS];

    /* Pan position */
    float    pan_x;     /* -1 (left) .. 1 (right) */
    float    pan_y;     /* -1 (back) .. 1 (front) */
    float    width;     /* Source stereo width */

    /* Input */
    float    input_l;
    float    input_r;
} wb_surround_inst;

static void init_speakers_5_1(speaker_t *spk) {
    /* ITU-R BS.775 positions */
    spk[0] = (speaker_t){-1.0f, 1.0f, 0.0f};   /* L */
    spk[1] = (speaker_t){ 0.0f, 1.0f, 0.0f};   /* C */
    spk[2] = (speaker_t){ 1.0f, 1.0f, 0.0f};   /* R */
    spk[3] = (speaker_t){-1.0f,-1.0f, 0.0f};   /* Ls */
    spk[4] = (speaker_t){ 1.0f,-1.0f, 0.0f};   /* Rs */
    spk[5] = (speaker_t){ 0.0f, 0.0f, 0.0f};   /* LFE */
}

static void init_speakers_7_1(speaker_t *spk) {
    init_speakers_5_1(spk);
    spk[5] = (speaker_t){-0.5f,-1.0f, 0.0f};   /* Lss */
    spk[6] = (speaker_t){ 0.5f,-1.0f, 0.0f};   /* Rss */
    spk[7] = (speaker_t){ 0.0f, 0.0f, 0.0f};   /* LFE */
}

void *wb_surround_create(uint32_t sr, surround_format_t format) {
    wb_surround_inst *sur = (wb_surround_inst *)calloc(1, sizeof(*sur));
    if (!sur) return NULL;
    sur->sr = sr;
    sur->format = format;
    sur->pan_x = 0.0f;
    sur->pan_y = 0.0f;
    sur->width = 1.0f;

    switch (format) {
    case SURROUND_5_1:
        sur->num_speakers = 6;
        init_speakers_5_1(sur->speakers);
        break;
    case SURROUND_7_1:
        sur->num_speakers = 8;
        init_speakers_7_1(sur->speakers);
        break;
    default:
        sur->num_speakers = 2;
        sur->speakers[0] = (speaker_t){-1.0f, 1.0f, 0.0f};
        sur->speakers[1] = (speaker_t){ 1.0f, 1.0f, 0.0f};
        break;
    }

    return sur;
}

void wb_surround_destroy(void *inst) { free(inst); }

void wb_surround_set_pan(void *inst, float x, float y) {
    wb_surround_inst *sur = (wb_surround_inst *)inst;
    if (!sur) return;
    sur->pan_x = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
    sur->pan_y = y < -1.0f ? -1.0f : (y > 1.0f ? 1.0f : y);
}

/* Compute gain vector for current pan position. */
static void compute_gains(wb_surround_inst *sur) {
    float src_x = sur->pan_x;
    float src_y = sur->pan_y;

    for (int i = 0; i < sur->num_speakers; i++) {
        float dx = src_x - sur->speakers[i].x;
        float dy = src_y - sur->speakers[i].y;
        float dist = sqrtf(dx * dx + dy * dy);

        /* Inverse distance weighting */
        float gain = 1.0f / (1.0f + dist * dist * 2.0f);
        sur->speakers[i].gain = gain;
    }

    /* Normalize */
    float total = 0;
    for (int i = 0; i < sur->num_speakers; i++) {
        total += sur->speakers[i].gain * sur->speakers[i].gain;
    }
    if (total > 0) {
        float norm = sqrtf(total);
        for (int i = 0; i < sur->num_speakers; i++) {
            sur->speakers[i].gain /= norm;
        }
    }
}

/* Process stereo input -> surround output.
 * in_l, in_r: stereo input
 * out: array of num_speakers float buffers
 * n: number of samples */
void wb_surround_process(void *inst, const float *in_l, const float *in_r,
                          float **out, uint32_t n) {
    wb_surround_inst *sur = (wb_surround_inst *)inst;
    if (!sur) return;

    compute_gains(sur);

    for (uint32_t s = 0; s < n; s++) {
        float l = in_l[s];
        float r = in_r[s];
        float mono = (l + r) * 0.5f;

        for (int spk = 0; spk < sur->num_speakers; spk++) {
            float gain = sur->speakers[spk].gain;

            /* Center channel gets mono */
            if ((sur->format == SURROUND_5_1 && spk == 1) ||
                (sur->format == SURROUND_7_1 && spk == 1)) {
                out[spk][s] = mono * gain;
            }
            /* LFE gets low-passed mono (simplified: just attenuate) */
            else if ((sur->format == SURROUND_5_1 && spk == 5) ||
                     (sur->format == SURROUND_7_1 && spk == 7)) {
                out[spk][s] = mono * gain * 0.5f;
            }
            /* Left speakers get more L, right speakers get more R */
            else if (sur->speakers[spk].x < 0) {
                out[spk][s] = l * gain;
            } else {
                out[spk][s] = r * gain;
            }
        }
    }
}

int wb_surround_get_num_speakers(void *inst) {
    wb_surround_inst *sur = (wb_surround_inst *)inst;
    return sur ? sur->num_speakers : 2;
}
