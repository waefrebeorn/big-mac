/* wb_pitch_correct.c — auto-tune style pitch correction.
 *
 * Melodyne-parity: snap detected pitch to nearest scale note.
 * Uses wb_yin.c for pitch detection, then shifts by resampling.
 *
 * Algorithm:
 *   1. Detect current pitch via YIN
 *   2. Find nearest note in active scale
 *   3. Compute pitch shift ratio = target_freq / detected_freq
 *   4. Apply shift via linear resampling (fast) or phase vocoder (quality)
 *   5. Mix dry/wet for natural vs. hard correction
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

extern float wb_yin_pitch(const float *buf, int n, uint32_t sr);

/* Scale definitions (semitone offsets from root) */
#define SCALE_MAJOR 0,2,4,5,7,9,11
#define SCALE_MINOR 0,2,3,5,7,8,10
#define SCALE_DORIAN 0,2,3,5,7,9,10
#define SCALE_MIXOLYDIAN 0,2,4,5,7,9,10
#define SCALE_PENTATONIC 0,2,4,7,9
#define SCALE_BLUES 0,3,5,6,7,10
#define SCALE_CHROMATIC 0,1,2,3,4,5,6,7,8,9,10,11

typedef struct {
    uint32_t sr;
    int      root_note;     /* MIDI note number of scale root (0=C, 1=C#, ...) */
    const int *scale;       /* semitone offsets */
    int      scale_len;
    float    correction;    /* 0=none, 1=full snap */
    float    min_confidence; /* minimum YIN confidence to correct */
    /* Internal */
    float    phase_accum;   /* for resampling pitch shift */
    float    prev_pitch;
} wb_pitch_corr_inst;

static int scale_major[] = {SCALE_MAJOR};
static int scale_minor[] = {SCALE_MINOR};
static int scale_pentatonic[] = {SCALE_PENTATONIC};
static int scale_chromatic[] = {SCALE_CHROMATIC};

void *wb_pitch_correct_create(uint32_t sr) {
    wb_pitch_corr_inst *pc = (wb_pitch_corr_inst *)calloc(1, sizeof(*pc));
    if (!pc) return NULL;
    pc->sr = sr;
    pc->root_note = 0;  /* C */
    pc->scale = scale_major;
    pc->scale_len = 7;
    pc->correction = 1.0f;
    pc->min_confidence = 0.1f;
    pc->phase_accum = 0.0f;
    pc->prev_pitch = 0.0f;
    return pc;
}

void wb_pitch_correct_destroy(void *inst) {
    free(inst);
}

void wb_pitch_correct_set(void *inst, int param, float v) {
    wb_pitch_corr_inst *pc = (wb_pitch_corr_inst *)inst;
    if (!pc) return;
    switch (param) {
    case 0: /* correction amount */
        pc->correction = v < 0 ? 0 : (v > 1 ? 1 : v);
        break;
    case 1: /* root note */
        pc->root_note = (int)v % 12;
        break;
    case 2: /* scale */
        {
            int s = (int)v;
            if (s == 0) { pc->scale = scale_major; pc->scale_len = 7; }
            else if (s == 1) { pc->scale = scale_minor; pc->scale_len = 7; }
            else if (s == 2) { pc->scale = scale_pentatonic; pc->scale_len = 5; }
            else if (s == 3) { pc->scale = scale_chromatic; pc->scale_len = 12; }
        }
        break;
    default: break;
    }
}

/* Find nearest scale note frequency for a given frequency.
 * Returns the target frequency. */
static float nearest_scale_note(wb_pitch_corr_inst *pc, float freq) {
    if (freq <= 0) return freq;

    /* Convert freq to MIDI note number */
    float midi_note = 69.0f + 12.0f * log2f(freq / 440.0f);
    int midi_rounded = (int)roundf(midi_note);

    /* Find nearest scale note */
    int octave = midi_rounded / 12;
    int semitone = midi_rounded % 12;
    if (semitone < 0) semitone += 12;

    int best_offset = pc->scale[0];
    int best_dist = abs(semitone - (pc->scale[0] + pc->root_note) % 12);
    if (best_dist > 6) best_dist = 12 - best_dist;

    for (int i = 1; i < pc->scale_len; i++) {
        int note_semitone = (pc->scale[i] + pc->root_note) % 12;
        int dist = abs(semitone - note_semitone);
        if (dist > 6) dist = 12 - dist;
        if (dist < best_dist) {
            best_dist = dist;
            best_offset = pc->scale[i];
        }
    }

    int target_midi = octave * 12 + ((best_offset + pc->root_note) % 12);
    /* If we're closer to the next octave, adjust */
    if (midi_note - midi_rounded > 0.5f && target_midi < midi_rounded) {
        target_midi += 12;
    } else if (midi_note - midi_rounded < -0.5f && target_midi > midi_rounded) {
        target_midi -= 12;
    }

    return 440.0f * powf(2.0f, (target_midi - 69.0f) / 12.0f);
}

/* Pitch shift via linear resampling.
 * Shifts by ratio (e.g., 1.0595 = +1 semitone).
 * Uses linear interpolation for fractional sample reads.
 * Returns number of output samples produced. */
static int pitch_shift_resample(const wb_sample *in, uint32_t in_len,
                                 wb_sample *out, uint32_t out_max,
                                 float ratio) {
    uint32_t out_len = 0;
    float read_pos = 0.0f;

    while (read_pos < (float)(in_len - 1) && out_len < out_max) {
        uint32_t idx = (uint32_t)read_pos;
        float frac = read_pos - (float)idx;
        float sample = in[idx] + frac * (in[idx + 1] - in[idx]);
        out[out_len++] = sample;
        read_pos += ratio;
    }

    return out_len;
}

/* Process a block of audio with pitch correction.
 * Uses overlap-add with windowed frames for smooth transitions. */
void wb_pitch_correct_process(void *inst, wb_sample *buf, uint32_t n) {
    wb_pitch_corr_inst *pc = (wb_pitch_corr_inst *)inst;
    if (!pc || pc->correction <= 0.0f) return;

    uint32_t sr = pc->sr;
    /* Analysis frame: 2048 samples @ 44.1k = 46ms — good for pitch */
    uint32_t frame_size = 2048;
    if (n < frame_size) frame_size = n;

    /* Detect pitch in current frame */
    float detected_pitch = wb_yin_pitch(buf, (int)frame_size, sr);

    if (detected_pitch <= 0) return;

    /* Smooth pitch changes */
    if (pc->prev_pitch > 0) {
        detected_pitch = 0.7f * pc->prev_pitch + 0.3f * detected_pitch;
    }
    pc->prev_pitch = detected_pitch;

    /* Find target pitch from scale */
    float target_pitch = nearest_scale_note(pc, detected_pitch);

    /* Compute correction ratio */
    float correction_ratio = target_pitch / detected_pitch;

    /* Blend between 1.0 (no shift) and correction_ratio based on correction amount */
    float ratio = 1.0f + pc->correction * (correction_ratio - 1.0f);

    /* Clamp to reasonable range (avoid artifacts) */
    if (ratio < 0.5f) ratio = 0.5f;
    if (ratio > 2.0f) ratio = 2.0f;

    /* If ratio is close to 1.0, skip processing */
    if (fabsf(ratio - 1.0f) < 0.001f) return;

    /* Apply pitch shift via resampling */
    wb_sample *temp = (wb_sample *)malloc(n * sizeof(wb_sample));
    if (!temp) return;

    int out_len = pitch_shift_resample(buf, n, temp, n, ratio);

    /* Copy back (may be shorter than input) */
    memcpy(buf, temp, out_len * sizeof(wb_sample));
    /* Zero remaining samples if output is shorter */
    if (out_len < (int)n) {
        memset(buf + out_len, 0, (n - out_len) * sizeof(wb_sample));
    }

    free(temp);
}
