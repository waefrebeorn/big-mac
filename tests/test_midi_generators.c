/* tests/test_midi_generators.c — test MIDI generators feature. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    void *gen = wb_midi_gen_create(44100);
    CHECK(gen != NULL);

    int notes[32], positions[32], velocities[32], durations[32];
    uint32_t seed;

    /* 1. Melody: all notes in C major scale */
    int n = wb_midi_gen_generate_melody(gen, 0, 0, 32, 60, 24, &seed, notes, positions, velocities, durations);
    CHECK(n == 32);
    int in_scale = 1;
    for (int i = 0; i < n; i++) {
        int pc = (notes[i] - 0) % 12;
        if (pc < 0) pc += 12;
        /* C major: 0,2,4,5,7,9,11 */
        int found = (pc==0||pc==2||pc==4||pc==5||pc==7||pc==9||pc==11);
        if (!found) { in_scale = 0; printf("    note[%d]=%d pc=%d not in C major\n", i, notes[i], pc); }
    }
    CHECK(in_scale);

    /* 2. Chord generation */
    int roots[8], types[8];
    int nc = wb_midi_gen_generate_chords(gen, 0, 0, 8, 0, &seed, roots, types);
    CHECK(nc == 8);
    int valid_types = 1;
    for (int i = 0; i < nc; i++) {
        if (types[i] < 0 || types[i] >= 7) valid_types = 0;
    }
    CHECK(valid_types);

    /* 3. Rhythm: correct number of steps */
    int hits[16], rhy_vel[16];
    int nr = wb_midi_gen_generate_rhythm(gen, 16, 4, 0.5f, 0.0f, &seed, hits, rhy_vel);
    CHECK(nr == 16);

    /* 4. Seed reproducibility */
    wb_midi_gen_set_seed(gen, 42);
    int notes1[16], pos1[16], vel1[16], dur1[16];
    wb_midi_gen_generate_melody(gen, 0, 0, 16, 60, 24, &seed, notes1, pos1, vel1, dur1);
    wb_midi_gen_set_seed(gen, 42);
    int notes2[16], pos2[16], vel2[16], dur2[16];
    wb_midi_gen_generate_melody(gen, 0, 0, 16, 60, 24, &seed, notes2, pos2, vel2, dur2);
    int reproducible = (memcmp(notes1, notes2, 16*sizeof(int)) == 0);
    CHECK(reproducible);

    /* 5. Density affects hit count */
    int hits_low[32], vel_low[32];
    wb_midi_gen_generate_rhythm(gen, 32, 4, 0.2f, 0.0f, &seed, hits_low, vel_low);
    int hits_high[32], vel_high[32];
    wb_midi_gen_generate_rhythm(gen, 32, 4, 0.8f, 0.0f, &seed, hits_high, vel_high);
    int count_low = 0, count_high = 0;
    for (int i = 0; i < 32; i++) { count_low += hits_low[i]; count_high += hits_high[i]; }
    CHECK(count_high > count_low);

    /* 6. Range constraint */
    int range_notes[16], range_pos[16], range_vel[16], range_dur[16];
    wb_midi_gen_generate_melody(gen, 0, 0, 16, 60, 12, &seed, range_notes, range_pos, range_vel, range_dur);
    int in_range = 1;
    for (int i = 0; i < 16; i++) {
        if (range_notes[i] < 54 || range_notes[i] > 66) in_range = 0;
    }
    CHECK(in_range);

    /* 7. Positions monotonically increasing */
    int mono = 1;
    for (int i = 1; i < n; i++) {
        if (positions[i] <= positions[i-1]) mono = 0;
    }
    CHECK(mono);

    wb_midi_gen_destroy(gen);

    printf("\nMIDI Generators: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
