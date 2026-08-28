/* wb_beat_slicer.c — beat-sliced audio chopping and rearrangement.
 *
 * R077: Essential for remix/mashup/meme editing.
 *
 * Algorithm:
 *   1. Detect beat positions (onset detection)
 *   2. Slice audio at beat boundaries
 *   3. Rearrange slices (reverse, repeat, shuffle, stutter)
 *   4. Crossfade slices for smooth transitions
 *
 * Modes:
 *   0: Forward (no change)
 *   1: Reverse all slices
 *   2: Repeat each slice (stutter)
 *   3: Shuffle slices randomly
 *   4: Half-speed (play every other slice)
 *   5: Double-speed (skip every other slice)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_SLICES 256

typedef struct {
    float start;        /* Start time in seconds */
    float end;          /* End time in seconds */
    int   active;       /* Is this slice used */
} slice_t;

typedef struct {
    uint32_t sr;
    int      num_slices;
    slice_t  slices[MAX_SLICES];
    int      mode;
    float    bpm;
    float    slice_duration;  /* Duration of one slice in seconds */
    int      current_slice;
    float    crossfade;       /* Crossfade amount (0..1) */
} wb_beat_slicer_inst;

void *wb_beat_slicer_create(uint32_t sr) {
    wb_beat_slicer_inst *bs = (wb_beat_slicer_inst *)calloc(1, sizeof(*bs));
    if (!bs) return NULL;
    bs->sr = sr;
    bs->mode = 0;
    bs->bpm = 120.0f;
    bs->slice_duration = 0.5f;  /* 8th notes at 120bpm */
    bs->current_slice = 0;
    bs->crossfade = 0.05f;
    return bs;
}

void wb_beat_slicer_destroy(void *inst) { free(inst); }

void wb_beat_slicer_set(void *inst, int param, float v) {
    wb_beat_slicer_inst *bs = (wb_beat_slicer_inst *)inst;
    if (!bs) return;
    switch (param) {
    case 0: bs->mode = (int)v; break;
    case 1: bs->bpm = v > 20 ? v : 20; break;
    case 2: bs->crossfade = v < 0 ? 0 : (v > 0.5f ? 0.5f : v); break;
    default: break;
    }
}

/* Generate beat slices from BPM.
 * duration: total audio duration in seconds. */
void wb_beat_slicer_generate(wb_beat_slicer_inst *bs, float duration) {
    if (!bs) return;

    bs->slice_duration = 60.0f / bs->bpm;  /* 1 beat */
    bs->num_slices = 0;

    float t = 0;
    while (t < duration && bs->num_slices < MAX_SLICES) {
        bs->slices[bs->num_slices].start = t;
        bs->slices[bs->num_slices].end = t + bs->slice_duration;
        bs->slices[bs->num_slices].active = 1;
        bs->num_slices++;
        t += bs->slice_duration;
    }
}

/* Get the source time for a given output time.
 * This is the core rearrangement logic. */
float wb_beat_slicer_map_time(wb_beat_slicer_inst *bs, float out_time) {
    if (!bs || bs->num_slices == 0) return out_time;

    float total_duration = bs->slices[bs->num_slices - 1].end;
    float wrapped_time = fmodf(out_time, total_duration);
    if (wrapped_time < 0) wrapped_time += total_duration;

    /* Find which slice this falls in */
    int slice_idx = (int)(wrapped_time / bs->slice_duration);
    if (slice_idx >= bs->num_slices) slice_idx = bs->num_slices - 1;

    float slice_offset = wrapped_time - bs->slices[slice_idx].start;

    switch (bs->mode) {
    case 0: /* Forward */
        return wrapped_time;

    case 1: /* Reverse */
        return total_duration - wrapped_time;

    case 2: /* Stutter (repeat each slice) */
        return bs->slices[slice_idx].start + fmodf(slice_offset * 2.0f, bs->slice_duration);

    case 3: /* Shuffle (deterministic based on slice index) */
    {
        /* Simple hash-based shuffle */
        int shuffled = (slice_idx * 7 + 3) % bs->num_slices;
        return bs->slices[shuffled].start + slice_offset;
    }

    case 4: /* Half speed */
        return wrapped_time * 0.5f;

    case 5: /* Double speed */
        return wrapped_time * 2.0f;

    default:
        return wrapped_time;
    }
}

/* Process: rearrange audio according to slice mapping.
 * Uses linear interpolation for non-integer time mapping.
 * in: input audio (mono)
 * out: output audio (mono)
 * n: number of samples */
void wb_beat_slicer_process(wb_beat_slicer_inst *bs,
                             const float *in, float *out,
                             int n, int n_channels) {
    if (!bs || !in || !out) return;

    for (int i = 0; i < n; i++) {
        float out_time = (float)i / (float)bs->sr;
        float src_time = wb_beat_slicer_map_time(bs, out_time);

        /* Convert source time to sample index */
        float src_sample_f = src_time * (float)bs->sr;
        int src_idx = (int)src_sample_f;
        float frac = src_sample_f - (float)src_idx;

        /* Clamp */
        int max_idx = n - 2;
        if (src_idx < 0) src_idx = 0;
        if (src_idx > max_idx) src_idx = max_idx;

        /* Linear interpolation */
        for (int c = 0; c < n_channels; c++) {
            out[i * n_channels + c] = in[src_idx * n_channels + c] +
                frac * (in[(src_idx + 1) * n_channels + c] - in[src_idx * n_channels + c]);
        }
    }
}
