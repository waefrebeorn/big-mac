/* wb_midi_generators.c — MIDI generators (Ableton Live 12 style).
 *
 * Generate melodies, chords, and rhythms from constraints.
 * Uses xorshift32 PRNG for reproducible generation.
 *
 * Pure C11. */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "wbus.h"

/* Scale intervals (semitone offsets from root) */
static const int scale_intervals[][12] = {
    {0,2,4,5,7,9,11},     /* Major */
    {0,2,3,5,7,8,10},     /* Minor */
    {0,2,3,5,7,8,11},     /* Harmonic minor */
    {0,2,3,5,7,9,11},     /* Melodic minor */
    {0,2,3,5,7,9,10},     /* Dorian */
    {0,1,3,5,7,8,10},     /* Phrygian */
    {0,2,4,6,7,9,11},     /* Lydian */
    {0,2,4,5,7,9,10},     /* Mixolydian */
    {0,2,4,7,9},          /* Pentatonic major */
    {0,3,5,7,10},         /* Pentatonic minor */
    {0,3,5,6,7,10},       /* Blues */
    {0,1,4,5,7,8,10},     /* Japanese */
    {0,1,4,5,7,8,11},     /* Flamenco */
    {0,2,4,6,8,10},       /* Whole tone */
    {0,2,3,5,6,8,9,11},   /* Diminished */
    {0,1,2,3,4,5,6,7,8,9,10,11}, /* Chromatic */
};
static const int scale_sizes[] = {7,7,7,7,7,7,7,7,5,5,6,7,7,6,8,12};

/* Chord types: root, third, fifth, [seventh] */
static const int chord_intervals[][4] = {
    {0,4,7,-1},   /* Major */
    {0,3,7,-1},   /* Minor */
    {0,3,6,-1},   /* Diminished */
    {0,4,8,-1},   /* Augmented */
    {0,4,7,11},   /* Major 7 */
    {0,3,7,10},   /* Minor 7 */
    {0,4,7,10},   /* Dominant 7 */
};

#define NUM_CHORD_TYPES 7

/* xorshift32 PRNG */
static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Random float 0..1 */
static float randf(uint32_t *state) {
    return (float)(xorshift32(state) & 0xFFFFFF) / (float)0x1000000;
}

/* Random int 0..n-1 */
static int randi(uint32_t *state, int n) {
    return (int)(xorshift32(state) % (uint32_t)n);
}

typedef struct {
    uint32_t seed;
} wb_midi_gen;

void *wb_midi_gen_create(uint32_t sr) {
    (void)sr;
    wb_midi_gen *g = (wb_midi_gen *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->seed = 1;
    return g;
}

void wb_midi_gen_destroy(void *gen) { free(gen); }

void wb_midi_gen_set_seed(void *gen, uint32_t seed) {
    wb_midi_gen *g = (wb_midi_gen *)gen;
    if (!g) return;
    g->seed = seed ? seed : 1;
}

int wb_midi_gen_generate_melody(void *gen, int scale_root, int scale_type,
                                int num_notes, int start_note, int range_semitones,
                                uint32_t *seed_out,
                                int *out_notes, int *out_positions,
                                int *out_velocities, int *out_durations) {
    wb_midi_gen *g = (wb_midi_gen *)gen;
    if (!g || !out_notes || !out_positions || !out_velocities || !out_durations)
        return -1;
    if (scale_type < 0 || scale_type >= 16) return -1;

    uint32_t state = g->seed;
    int n = scale_sizes[scale_type];
    const int *intervals = scale_intervals[scale_type];

    int current = start_note;
    double pos = 0;

    for (int i = 0; i < num_notes; i++) {
        /* Pick a scale degree */
        int degree = randi(&state, n);
        int octave_offset = randi(&state, 3) - 1; /* -1, 0, +1 octave */
        int note = scale_root + intervals[degree] + octave_offset * 12;

        /* Clamp to range */
        int min_note = start_note - range_semitones / 2;
        int max_note = start_note + range_semitones / 2;
        if (note < min_note) note = min_note;
        if (note > max_note) note = max_note;
        if (note < 0) note = 0;
        if (note > 127) note = 127;

        out_notes[i] = note;

        /* Duration: 1/4, 1/8, 1/16, 1/2 */
        float dur_rand = randf(&state);
        double dur;
        if (dur_rand < 0.3) dur = 0.25;
        else if (dur_rand < 0.6) dur = 0.5;
        else if (dur_rand < 0.85) dur = 1.0;
        else dur = 2.0;

        out_positions[i] = (int)(pos * 480); /* ticks */
        pos += dur;

        /* Velocity with humanization */
        int vel = 80 + randi(&state, 40) - 20;
        if (vel < 40) vel = 40;
        if (vel > 127) vel = 127;
        out_velocities[i] = vel;
        out_durations[i] = (int)(dur * 480); /* ticks at 480 PPQ */
    }

    g->seed = state;
    if (seed_out) *seed_out = state;
    return num_notes;
}

int wb_midi_gen_generate_chords(void *gen, int scale_root, int scale_type,
                                 int num_chords, int progression_type,
                                 uint32_t *seed_out,
                                 int *out_roots, int *out_types) {
    wb_midi_gen *g = (wb_midi_gen *)gen;
    if (!g || !out_roots || !out_types) return -1;
    if (scale_type < 0 || scale_type >= 16) return -1;

    uint32_t state = g->seed;
    int n = scale_sizes[scale_type];
    const int *intervals = scale_intervals[scale_type];

    /* Common progressions (scale degree indices) */
    static const int prog_1564[] = {0,4,5,3};  /* I-V-vi-IV */
    static const int prog_251[]  = {1,4,0};     /* ii-V-I */
    static const int prog_blues[]= {0,0,0,0,3,3,0,0,4,3,0,0}; /* 12-bar blues */
    static const int prog_pop[]   = {0,3,4,0};  /* I-IV-V-I */

    const int *prog = NULL;
    int prog_len = 0;
    switch (progression_type) {
        case 0: prog = prog_1564; prog_len = 4; break;
        case 1: prog = prog_251; prog_len = 3; break;
        case 2: prog = prog_blues; prog_len = 12; break;
        case 3: prog = prog_pop; prog_len = 4; break;
        default: prog = prog_1564; prog_len = 4; break;
    }

    for (int i = 0; i < num_chords; i++) {
        int degree_idx;
        if (progression_type >= 0 && progression_type <= 3) {
            degree_idx = prog[i % prog_len];
        } else {
            degree_idx = randi(&state, n);
        }
        if (degree_idx >= n) degree_idx = degree_idx % n;

        out_roots[i] = scale_root + intervals[degree_idx];
        out_types[i] = randi(&state, NUM_CHORD_TYPES);
    }

    g->seed = state;
    if (seed_out) *seed_out = state;
    return num_chords;
}

int wb_midi_gen_generate_rhythm(void *gen, int num_steps, int division,
                                 float density, float swing,
                                 uint32_t *seed_out,
                                 int *out_hits, int *out_velocities) {
    wb_midi_gen *g = (wb_midi_gen *)gen;
    if (!g || !out_hits || !out_velocities) return -1;
    if (density < 0) density = 0;
    if (density > 1) density = 1;

    uint32_t state = g->seed;

    for (int i = 0; i < num_steps; i++) {
        float p = randf(&state);
        if (p < density) {
            out_hits[i] = 1;
            /* Accent on beat 1 of each bar */
            int is_downbeat = (i % division) == 0;
            int vel = is_downbeat ? 110 : 80;
            /* Ghost notes */
            if (randf(&state) < 0.15) vel = 50;
            /* Velocity humanization */
            vel += randi(&state, 20) - 10;
            if (vel < 20) vel = 20;
            if (vel > 127) vel = 127;
            out_velocities[i] = vel;
        } else {
            out_hits[i] = 0;
            out_velocities[i] = 0;
        }
    }

    g->seed = state;
    if (seed_out) *seed_out = state;
    return num_steps;
}
