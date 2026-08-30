/* wb_midi_chordgen.c — MIDI chord generator.
 *
 * R078 H1: Generate chords from single MIDI notes.
 *
 * Chord types: major, minor, dim, aug, sus2, sus4, maj7, min7, dom7, 9th
 * Voicings: close, open, drop2, drop3
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_CHORD_NOTES 8

typedef enum {
    CHORD_MAJOR = 0,
    CHORD_MINOR,
    CHORD_DIMINISHED,
    CHORD_AUGMENTED,
    CHORD_SUS2,
    CHORD_SUS4,
    CHORD_MAJOR7,
    CHORD_MINOR7,
    CHORD_DOMINANT7,
    CHORD_MAJOR9,
    CHORD_MINOR9,
    CHORD_DOMINANT9,
    CHORD_ADD9,
    CHORD_COUNT
} chord_type_t;

/* Semitone intervals from root for each chord type */
static const int chord_intervals[CHORD_COUNT][6] = {
    {0, 4, 7, -1, -1, -1},           /* Major */
    {0, 3, 7, -1, -1, -1},           /* Minor */
    {0, 3, 6, -1, -1, -1},           /* Diminished */
    {0, 4, 8, -1, -1, -1},           /* Augmented */
    {0, 2, 7, -1, -1, -1},           /* Sus2 */
    {0, 5, 7, -1, -1, -1},           /* Sus4 */
    {0, 4, 7, 11, -1, -1},           /* Major 7 */
    {0, 3, 7, 10, -1, -1},           /* Minor 7 */
    {0, 4, 7, 10, -1, -1},           /* Dominant 7 */
    {0, 4, 7, 11, 14, -1},           /* Major 9 */
    {0, 3, 7, 10, 14, -1},           /* Minor 9 */
    {0, 4, 7, 10, 14, -1},           /* Dominant 9 */
    {0, 4, 7, 14, -1, -1},           /* Add9 */
};

static const char *chord_names[] = {
    "maj", "min", "dim", "aug", "sus2", "sus4",
    "maj7", "min7", "7", "maj9", "min9", "9", "add9"
};

typedef struct {
    int      notes[MAX_CHORD_NOTES];
    int      num_notes;
    int      velocity;
    uint32_t duration_ms;
} chord_result_t;

/* Generate a chord from a root MIDI note.
 * root_note: MIDI note number (0-127)
 * type: chord type
 * output: array of MIDI note numbers (up to MAX_CHORD_NOTES)
 * Returns number of notes in chord. */
int wb_chord_generate(int root_note, chord_type_t type, int *output) {
    if (type >= CHORD_COUNT || !output) return 0;

    int count = 0;
    for (int i = 0; i < 6; i++) {
        int interval = chord_intervals[type][i];
        if (interval < 0) break;
        int note = root_note + interval;
        if (note <= 127) {
            output[count++] = note;
        }
    }
    return count;
}

/* Generate a chord with voicing.
 * voicing: 0=close, 1=open, 2=drop2, 3=drop3 */
int wb_chord_generate_voiced(int root_note, chord_type_t type, int voicing,
                               int *output) {
    int notes[6];
    int count = wb_chord_generate(root_note, type, notes);
    if (count == 0) return 0;

    switch (voicing) {
    case 0: /* Close voicing */
        memcpy(output, notes, count * sizeof(int));
        return count;

    case 1: /* Open voicing: spread across octaves */
        for (int i = 0; i < count; i++) {
            output[i] = notes[i] + (i % 2) * 12;
            if (output[i] > 127) output[i] = 127;
        }
        return count;

    case 2: /* Drop 2: lower the second-highest note an octave */
        memcpy(output, notes, count * sizeof(int));
        if (count >= 2) {
            output[count - 2] -= 12;
            if (output[count - 2] < 0) output[count - 2] = 0;
        }
        return count;

    case 3: /* Drop 3: lower the third-highest note an octave */
        memcpy(output, notes, count * sizeof(int));
        if (count >= 3) {
            output[count - 3] -= 12;
            if (output[count - 3] < 0) output[count - 3] = 0;
        }
        return count;

    default:
        memcpy(output, notes, count * sizeof(int));
        return count;
    }
}

/* Get chord name. */
const char* wb_chordgen_get_name(chord_type_t type) {
    if (type < CHORD_COUNT) return chord_names[type];
    return "?";
}

/* Detect chord from a set of MIDI notes.
 * notes: array of MIDI note numbers
 * num_notes: number of notes
 * Returns detected chord type and root via pointers. */
int wb_chordgen_detect(const int *notes, int num_notes, int *root_out,
                      chord_type_t *type_out) {
    if (!notes || num_notes < 2) return -1;

    /* Try each note as potential root */
    for (int r = 0; r < num_notes; r++) {
        int root = notes[r] % 12;

        /* Build interval set from root */
        int intervals[12] = {0};
        for (int i = 0; i < num_notes; i++) {
            int interval = (notes[i] - root) % 12;
            if (interval < 0) interval += 12;
            intervals[interval] = 1;
        }

        /* Match against chord templates */
        for (int type = 0; type < CHORD_COUNT; type++) {
            int match = 1;
            int template_count = 0;

            for (int i = 0; i < 6; i++) {
                int iv = chord_intervals[type][i];
                if (iv < 0) break;
                template_count++;
                if (!intervals[iv % 12]) {
                    match = 0;
                    break;
                }
            }

            if (match && template_count <= num_notes) {
                if (root_out) *root_out = root;
                if (type_out) *type_out = type;
                return 0;
            }
        }
    }

    return -1;  /* No match */
}
