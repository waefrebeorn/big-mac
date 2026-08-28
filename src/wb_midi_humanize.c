/* wb_midi_humanize.c — MIDI humanization (timing/velocity randomization).
 *
 * R077: Add natural feel to quantized MIDI.
 *
 * Algorithm:
 *   - Timing offset: Gaussian or uniform random ±ms
 *   - Velocity offset: random ±units
 *   - Note length: random ±%
 *   - Optional: Perlin noise for natural drift
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    float    timing_ms;       /* Max timing variation in ms */
    float    velocity_amt;    /* Max velocity variation (0-127) */
    float    length_amt;      /* Max note length variation (0..1) */
    unsigned int seed;
    int      use_gaussian;    /* 1=Gaussian, 0=uniform */
} wb_midi_humanize_inst;

static unsigned int mh_rng(unsigned int *seed) {
    *seed = *seed * 1103515245u + 12345u;
    return *seed;
}

/* Box-Muller transform for Gaussian random */
static float mh_gaussian(unsigned int *seed) {
    float u1 = (float)(mh_rng(seed) % 10000) / 10000.0f + 0.0001f;
    float u2 = (float)(mh_rng(seed) % 10000) / 10000.0f;
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * 3.14159265f * u2);
}

void *wb_midi_humanize_create(void) {
    wb_midi_humanize_inst *mh = (wb_midi_humanize_inst *)calloc(1, sizeof(*mh));
    if (!mh) return NULL;
    mh->timing_ms = 15.0f;
    mh->velocity_amt = 10.0f;
    mh->length_amt = 0.05f;
    mh->seed = 0x12345678;
    mh->use_gaussian = 1;
    return mh;
}

void wb_midi_humanize_destroy(void *inst) { free(inst); }

void wb_midi_humanize_set(void *inst, int param, float v) {
    wb_midi_humanize_inst *mh = (wb_midi_humanize_inst *)inst;
    if (!mh) return;
    switch (param) {
    case 0: mh->timing_ms = v > 0 ? v : 0; break;
    case 1: mh->velocity_amt = v > 0 ? (v > 127 ? 127 : v) : 0; break;
    case 2: mh->length_amt = v < 0 ? 0 : (v > 0.5f ? 0.5f : v); break;
    case 3: mh->use_gaussian = (int)v; break;
    default: break;
    }
}

/* Humanize a MIDI note event.
 * note_pos: position in samples
 * velocity: note velocity (0-127)
 * length: note length in samples
 * Returns modified values via pointers. */
void wb_midi_humanize_note(wb_midi_humanize_inst *mh,
                            int *note_pos, int *velocity, int *length,
                            int sample_rate, float bpm) {
    if (!mh) return;

    /* Timing offset */
    float timing_offset = 0;
    if (mh->timing_ms > 0) {
        float max_offset = mh->timing_ms * (float)sample_rate / 1000.0f;
        if (mh->use_gaussian) {
            timing_offset = mh_gaussian(&mh->seed) * max_offset * 0.5f;
        } else {
            float r = (float)(mh_rng(&mh->seed) % 1000) / 1000.0f - 0.5f;
            timing_offset = r * max_offset;
        }
    }

    /* Velocity offset */
    int vel_offset = 0;
    if (mh->velocity_amt > 0) {
        if (mh->use_gaussian) {
            vel_offset = (int)(mh_gaussian(&mh->seed) * mh->velocity_amt * 0.5f);
        } else {
            float r = (float)(mh_rng(&mh->seed) % 1000) / 1000.0f - 0.5f;
            vel_offset = (int)(r * mh->velocity_amt * 2.0f);
        }
    }

    /* Length offset */
    int len_offset = 0;
    if (mh->length_amt > 0 && *length > 0) {
        float r = (float)(mh_rng(&mh->seed) % 1000) / 1000.0f - 0.5f;
        len_offset = (int)(r * (float)(*length) * mh->length_amt * 2.0f);
    }

    /* Apply */
    *note_pos += (int)timing_offset;
    if (*note_pos < 0) *note_pos = 0;

    *velocity += vel_offset;
    if (*velocity < 1) *velocity = 1;
    if (*velocity > 127) *velocity = 127;

    *length += len_offset;
    if (*length < 1) *length = 1;
}
