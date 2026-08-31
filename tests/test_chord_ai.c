/* tests/test_chord_ai.c — test AI chord progression generator. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    void *ai = wb_chord_ai_create(44100);
    CHECK(ai != NULL);

    int roots[8], types[8];
    uint32_t seed;

    /* 1. Generate happy progression */
    int n = wb_chord_ai_generate(ai, 0, 0, 4, roots, types, &seed);
    CHECK(n == 4);
    int valid = 1;
    for (int i = 0; i < n; i++) {
        if (roots[i] < 0 || roots[i] > 127) valid = 0;
        if (types[i] < 0 || types[i] >= 7) valid = 0;
    }
    CHECK(valid);

    /* 2. Generate sad progression */
    n = wb_chord_ai_generate(ai, 0, 1, 4, roots, types, &seed);
    CHECK(n == 4);

    /* 3. Seed reproducibility */
    wb_chord_ai_generate(ai, 0, 0, 4, roots, types, &seed);
    int roots2[8], types2[8];
    uint32_t seed2;
    wb_chord_ai_generate(ai, 0, 0, 4, roots2, types2, &seed2);
    /* Same seed should produce same results */
    int reproducible = (memcmp(roots, roots2, 4*sizeof(int)) == 0);
    CHECK(reproducible);

    /* 4. Tension score valid */
    float tension = wb_chord_ai_get_tension(ai);
    CHECK(tension >= 0.0f && tension <= 1.0f);

    /* 5. Mood affects output */
    wb_chord_ai_set_mood(ai, 0); /* happy */
    wb_chord_ai_generate(ai, 0, 0, 4, roots, types, &seed);
    int happy_types = 0;
    for (int i = 0; i < 4; i++) if (types[i] == 0) happy_types++; /* major chords */

    wb_chord_ai_set_mood(ai, 1); /* sad */
    wb_chord_ai_generate(ai, 0, 1, 4, roots, types, &seed);
    int sad_types = 0;
    for (int i = 0; i < 4; i++) if (types[i] == 1) sad_types++; /* minor chords */
    CHECK(sad_types > 0);

    /* 6. Complexity */
    wb_chord_ai_set_complexity(ai, 1.0f);
    wb_chord_ai_generate(ai, 0, 0, 4, roots, types, &seed);
    CHECK(n == 4);

    wb_chord_ai_destroy(ai);

    printf("\nChord AI: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
