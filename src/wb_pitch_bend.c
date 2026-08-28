/* wb_pitch_bend.c — continuous pitch bending / sliding.
 *
 * R077: YTP/meme essential — smooth pitch slides, not just shifts.
 *
 * Algorithm:
 *   Phase accumulator with variable increment.
 *   Pitch bend = modify increment in real-time.
 *   Supports: slide up, slide down, vibrato, scare chord (rapid oscillation).
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;
    float    base_freq;       /* Base frequency */
    float    bend_semitones;  /* Current pitch bend in semitones (-12 to +12) */
    float    bend_rate;       /* Bend speed (semitones/sec) */
    float    bend_target;     /* Target bend amount */
    float    vibrato_rate;    /* Vibrato LFO rate (Hz) */
    float    vibrato_depth;   /* Vibrato depth (semitones) */
    float    vibrato_phase;   /* Vibrato LFO phase */
    int      bend_direction;  /* +1 = sliding up, -1 = sliding down, 0 = hold */
} wb_pitch_bend_inst;

void *wb_pitch_bend_create(uint32_t sr) {
    wb_pitch_bend_inst *pb = (wb_pitch_bend_inst *)calloc(1, sizeof(*pb));
    if (!pb) return NULL;
    pb->sr = sr;
    pb->base_freq = 440.0f;
    pb->bend_semitones = 0.0f;
    pb->bend_rate = 12.0f;  /* 1 octave/sec default */
    pb->bend_target = 0.0f;
    pb->vibrato_rate = 5.0f;
    pb->vibrato_depth = 0.0f;
    pb->vibrato_phase = 0.0f;
    pb->bend_direction = 0;
    return pb;
}

void wb_pitch_bend_destroy(void *inst) {
    free(inst);
}

void wb_pitch_bend_set(void *inst, int param, float v) {
    wb_pitch_bend_inst *pb = (wb_pitch_bend_inst *)inst;
    if (!pb) return;
    switch (param) {
    case 0: pb->bend_semitones = v; break;
    case 1: pb->bend_rate = v > 0.1f ? v : 0.1f; break;
    case 2: pb->bend_target = v; break;
    case 3: pb->vibrato_rate = v; break;
    case 4: pb->vibrato_depth = v; break;
    default: break;
    }
}

/* Start a pitch slide */
void wb_pitch_bend_slide(void *inst, float target_semitones, float rate) {
    wb_pitch_bend_inst *pb = (wb_pitch_bend_inst *)inst;
    if (!pb) return;
    pb->bend_target = target_semitones;
    pb->bend_rate = rate > 0.1f ? rate : 1.0f;
    pb->bend_direction = (target_semitones > pb->bend_semitones) ? 1 : -1;
}

/* Process: get the current pitch ratio for this sample.
 * Caller uses this to resample their audio. */
float wb_pitch_bend_get_ratio(wb_pitch_bend_inst *pb) {
    if (!pb) return 1.0f;

    /* Update bend towards target */
    if (pb->bend_direction != 0) {
        float step = pb->bend_rate / (float)pb->sr;
        if (pb->bend_direction > 0) {
            pb->bend_semitones += step;
            if (pb->bend_semitones >= pb->bend_target) {
                pb->bend_semitones = pb->bend_target;
                pb->bend_direction = 0;
            }
        } else {
            pb->bend_semitones -= step;
            if (pb->bend_semitones <= pb->bend_target) {
                pb->bend_semitones = pb->bend_target;
                pb->bend_direction = 0;
            }
        }
    }

    /* Add vibrato */
    float total_bend = pb->bend_semitones;
    if (pb->vibrato_depth > 0) {
        pb->vibrato_phase += 2.0f * 3.14159265f * pb->vibrato_rate / (float)pb->sr;
        if (pb->vibrato_phase > 2.0f * 3.14159265f) pb->vibrato_phase -= 2.0f * 3.14159265f;
        total_bend += pb->vibrato_depth * sinf(pb->vibrato_phase);
    }

    /* Convert semitones to ratio: 2^(semitones/12) */
    return powf(2.0f, total_bend / 12.0f);
}

/* Process a block: pitch-shift the input using current bend.
 * Uses linear interpolation resampling. */
void wb_pitch_bend_process(void *inst, const float *in, float *out,
                            int n_frames, int n_channels) {
    wb_pitch_bend_inst *pb = (wb_pitch_bend_inst *)inst;
    if (!pb) { memcpy(out, in, n_frames * n_channels * sizeof(float)); return; }

    /* Simple implementation: apply current ratio to all samples */
    /* For a full implementation, use a ring buffer + variable read pointer */
    float ratio = wb_pitch_bend_get_ratio(pb);

    /* Pitch shift by resampling (read pointer moves at 'ratio' speed) */
    static double read_pos = 0;
    for (int i = 0; i < n_frames; i++) {
        int idx = (int)read_pos;
        float frac = (float)(read_pos - (double)idx);

        for (int c = 0; c < n_channels; c++) {
            if (idx + 1 < n_frames) {
                out[i * n_channels + c] = in[idx * n_channels + c] +
                    frac * (in[(idx + 1) * n_channels + c] - in[idx * n_channels + c]);
            } else {
                out[i * n_channels + c] = in[idx * n_channels + c];
            }
        }

        read_pos += ratio;
        if (read_pos >= n_frames) read_pos = 0;
    }
}

/* Preset: "Scare chord" — rapid pitch oscillation (horror effect) */
void wb_pitch_bend_scare_chord(void *inst) {
    wb_pitch_bend_set(inst, 3, 15.0f);   /* fast vibrato */
    wb_pitch_bend_set(inst, 4, 3.0f);    /* 3 semitones depth */
}

/* Preset: "Chipmunk" — shift up 1 octave */
void wb_pitch_bend_chipmunk(void *inst) {
    wb_pitch_bend_set(inst, 0, 12.0f);
    wb_pitch_bend_set(inst, 3, 0.0f);  /* no vibrato */
    wb_pitch_bend_set(inst, 4, 0.0f);
}

/* Preset: "Deep voice" — shift down 1 octave */
void wb_pitch_bend_deep(void *inst) {
    wb_pitch_bend_set(inst, 0, -12.0f);
    wb_pitch_bend_set(inst, 3, 0.0f);
    wb_pitch_bend_set(inst, 4, 0.0f);
}

/* Preset: "YTP slide" — slide up 2 octaves quickly */
void wb_pitch_bend_ytp_slide(void *inst) {
    wb_pitch_bend_slide(inst, 24.0f, 48.0f);  /* 2 octaves in ~0.5 sec */
}
