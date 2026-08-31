/* tests/test_melody_ai.c — test AI melody composer feature. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

/* Check if a note is in the given scale (by pitch class) */
static int is_in_scale(int note, int root, const int *scale_pcs, int num_pcs) {
    int pc = ((note - root) % 12 + 12) % 12;
    for (int i = 0; i < num_pcs; i++) {
        if (scale_pcs[i] == pc) return 1;
    }
    return 0;
}

int main(void) {
    int notes[256], positions[256], durations[256], velocities[256];

    /* 1. Create/destroy */
    void *ai = wb_melody_ai_create(44100);
    CHECK(ai != NULL);
    wb_melody_ai_destroy(ai);
    CHECK(1); /* destroy didn't crash */

    /* Re-create for remaining tests */
    ai = wb_melody_ai_create(44100);
    CHECK(ai != NULL);

    /* 2. Compose happy melody */
    wb_melody_ai_set_mood(ai, 0); /* happy */
    wb_melody_ai_set_tempo(ai, 120.0f);
    int n_happy = wb_melody_ai_compose(ai, 0 /* C */, WB_SCALE_MAJOR, 0 /* happy */,
                                        4 /* bars */, 480 /* ppq */,
                                        notes, positions, durations, velocities, 256);
    CHECK(n_happy > 0);
    printf("    happy melody: %d notes\n", n_happy);

    /* 3. Compose sad melody */
    wb_melody_ai_set_mood(ai, 1); /* sad */
    wb_melody_ai_set_tempo(ai, 80.0f);
    int n_sad = wb_melody_ai_compose(ai, 0 /* C */, WB_SCALE_MINOR, 1 /* sad */,
                                      4, 480,
                                      notes, positions, durations, velocities, 256);
    CHECK(n_sad > 0);
    printf("    sad melody: %d notes\n", n_sad);

    /* 4. Note count > 0 for all moods */
    for (int mood = 0; mood < 4; mood++) {
        wb_melody_ai_set_mood(ai, mood);
        int n = wb_melody_ai_compose(ai, 0, WB_SCALE_MAJOR, mood, 2, 480,
                                      notes, positions, durations, velocities, 256);
        printf("    mood %d: %d notes\n", mood, n);
        CHECK(n > 0);
    }

    /* 5. All notes in scale (C major) */
    wb_melody_ai_set_mood(ai, 0);
    int n = wb_melody_ai_compose(ai, 0, WB_SCALE_MAJOR, 0, 4, 480,
                                  notes, positions, durations, velocities, 256);
    /* C major pitch classes: 0,2,4,5,7,9,11 */
    int cm_pcs[] = {0, 2, 4, 5, 7, 9, 11};
    int all_in_scale = 1;
    for (int i = 0; i < n; i++) {
        if (!is_in_scale(notes[i], 0, cm_pcs, 7)) {
            all_in_scale = 0;
            printf("    note[%d]=%d not in C major\n", i, notes[i]);
        }
    }
    CHECK(all_in_scale);

    /* Also check C minor */
    wb_melody_ai_set_mood(ai, 1);
    n = wb_melody_ai_compose(ai, 0, WB_SCALE_MINOR, 1, 4, 480,
                              notes, positions, durations, velocities, 256);
    /* C minor pitch classes: 0,2,3,5,7,8,10 */
    int cmin_pcs[] = {0, 2, 3, 5, 7, 8, 10};
    all_in_scale = 1;
    for (int i = 0; i < n; i++) {
        if (!is_in_scale(notes[i], 0, cmin_pcs, 7)) {
            all_in_scale = 0;
            printf("    note[%d]=%d not in C minor\n", i, notes[i]);
        }
    }
    CHECK(all_in_scale);

    /* 6. Tempo affects density: higher tempo = more notes */
    wb_melody_ai_set_mood(ai, 2); /* energetic — most sensitive to tempo */
    wb_melody_ai_set_tempo(ai, 200.0f);
    int n_fast = wb_melody_ai_compose(ai, 0, WB_SCALE_BLUES, 2, 4, 480,
                                       notes, positions, durations, velocities, 256);

    /* Reset seed by recreating */
    wb_melody_ai_destroy(ai);
    ai = wb_melody_ai_create(44100);

    wb_melody_ai_set_mood(ai, 2); /* energetic */
    wb_melody_ai_set_tempo(ai, 60.0f);
    int n_slow = wb_melody_ai_compose(ai, 0, WB_SCALE_BLUES, 2, 4, 480,
                                       notes, positions, durations, velocities, 256);

    printf("    fast tempo (200): %d notes, slow tempo (60): %d notes\n", n_fast, n_slow);
    CHECK(n_fast >= n_slow);

    /* 7. Range constraint respected */
    wb_melody_ai_set_range(ai, 60, 72); /* C4 to C5 */
    wb_melody_ai_set_mood(ai, 0);
    n = wb_melody_ai_compose(ai, 0, WB_SCALE_MAJOR, 0, 4, 480,
                              notes, positions, durations, velocities, 256);
    int in_range = 1;
    for (int i = 0; i < n; i++) {
        if (notes[i] < 60 || notes[i] > 72) {
            in_range = 0;
            printf("    note[%d]=%d out of range [60,72]\n", i, notes[i]);
        }
    }
    CHECK(in_range);

    /* 8. Positions monotonically increasing */
    wb_melody_ai_set_range(ai, 48, 84);
    wb_melody_ai_set_mood(ai, 0);
    n = wb_melody_ai_compose(ai, 0, WB_SCALE_MAJOR, 0, 4, 480,
                              notes, positions, durations, velocities, 256);
    int mono = 1;
    for (int i = 1; i < n; i++) {
        if (positions[i] <= positions[i-1]) {
            mono = 0;
            printf("    position[%d]=%d <= position[%d]=%d\n", i, positions[i], i-1, positions[i-1]);
        }
    }
    CHECK(mono);

    /* 9. Velocities in valid MIDI range */
    int vel_ok = 1;
    for (int i = 0; i < n; i++) {
        if (velocities[i] < 0 || velocities[i] > 127) {
            vel_ok = 0;
            printf("    velocity[%d]=%d out of range\n", i, velocities[i]);
        }
    }
    CHECK(vel_ok);

    /* 10. Durations > 0 */
    int dur_ok = 1;
    for (int i = 0; i < n; i++) {
        if (durations[i] <= 0) {
            dur_ok = 0;
            printf("    duration[%d]=%d <= 0\n", i, durations[i]);
        }
    }
    CHECK(dur_ok);

    wb_melody_ai_destroy(ai);

    printf("\nMelody AI: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}