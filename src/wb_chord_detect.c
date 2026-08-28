/* wb_chord_detect.c — chord detection from MIDI notes.
 *
 * R077: Detect chords from MIDI note input.
 *
 * Algorithm:
 *   1. Build pitch class profile (12 bins) from active notes
 *   2. Correlate against chord templates (major, minor, dim, aug, 7th, etc.)
 *   3. Return best match with root note and chord type
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define NUM_CHORD_TYPES 12

typedef enum {
    CHORD_MAJOR = 0,
    CHORD_MINOR,
    CHORD_DIMINISHED,
    CHORD_AUGMENTED,
    CHORD_MAJOR7,
    CHORD_MINOR7,
    CHORD_DOMINANT7,
    CHORD_SUSPENDED2,
    CHORD_SUSPENDED4,
    CHORD_MAJOR6,
    CHORD_MINOR6,
    CHORD_UNKNOWN
} chord_type_t;

typedef struct {
    int root;            /* 0=C, 1=C#, ... 11=B */
    chord_type_t type;
    float confidence;
} chord_result_t;

/* Chord templates: semitone offsets from root */
static const int chord_templates[NUM_CHORD_TYPES][6] = {
    {0, 4, 7, -1, -1, -1},           /* Major */
    {0, 3, 7, -1, -1, -1},           /* Minor */
    {0, 3, 6, -1, -1, -1},           /* Diminished */
    {0, 4, 8, -1, -1, -1},           /* Augmented */
    {0, 4, 7, 11, -1, -1},           /* Major 7 */
    {0, 3, 7, 10, -1, -1},           /* Minor 7 */
    {0, 4, 7, 10, -1, -1},           /* Dominant 7 */
    {0, 2, 7, -1, -1, -1},           /* Suspended 2 */
    {0, 5, 7, -1, -1, -1},           /* Suspended 4 */
    {0, 4, 7, 9, -1, -1},            /* Major 6 */
    {0, 3, 7, 9, -1, -1},            /* Minor 6 */
    {0, -1, -1, -1, -1, -1},         /* Unknown */
};

static const char *chord_names[] = {
    "Major", "Minor", "Dim", "Aug", "Maj7", "Min7", "Dom7",
    "Sus2", "Sus4", "Maj6", "Min6", "Unknown"
};

static const char *note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/* Detect chord from active MIDI notes.
 * notes: array of active MIDI note numbers (0-127)
 * num_notes: number of active notes
 * result: output chord detection result */
int wb_chord_detect(const int *notes, int num_notes, chord_result_t *result) {
    if (!result) return -1;
    if (!notes || num_notes < 2) {
        result->root = 0;
        result->type = CHORD_UNKNOWN;
        result->confidence = 0;
        return 0;
    }

    /* Build pitch class profile */
    float pcp[12] = {0};
    for (int i = 0; i < num_notes; i++) {
        int pc = notes[i] % 12;
        pcp[pc] += 1.0f;
    }

    /* Normalize */
    float max_val = 0;
    for (int i = 0; i < 12; i++) {
        if (pcp[i] > max_val) max_val = pcp[i];
    }
    if (max_val > 0) {
        for (int i = 0; i < 12; i++) pcp[i] /= max_val;
    }

    /* Find best matching chord */
    float best_score = -1;
    int best_root = 0;
    int best_type = CHORD_UNKNOWN;

    for (int root = 0; root < 12; root++) {
        for (int type = 0; type < NUM_CHORD_TYPES - 1; type++) {
            float score = 0;
            int template_notes = 0;

            /* Check how many template notes are present */
            for (int n = 0; n < 6; n++) {
                int interval = chord_templates[type][n];
                if (interval < 0) break;
                int pc = (root + interval) % 12;
                if (pcp[pc] > 0.1f) {
                    score += pcp[pc];
                    template_notes++;
                }
            }

            /* Penalize extra notes not in template */
            for (int pc = 0; pc < 12; pc++) {
                int in_template = 0;
                for (int n = 0; n < 6; n++) {
                    int interval = chord_templates[type][n];
                    if (interval < 0) break;
                    if ((root + interval) % 12 == pc) {
                        in_template = 1;
                        break;
                    }
                }
                if (!in_template && pcp[pc] > 0.1f) {
                    score -= pcp[pc] * 0.5f;
                }
            }

            if (score > best_score) {
                best_score = score;
                best_root = root;
                best_type = type;
            }
        }
    }

    result->root = best_root;
    result->type = best_type;
    result->confidence = best_score / (float)num_notes;
    if (result->confidence > 1.0f) result->confidence = 1.0f;

    return 0;
}

/* Get chord name string. */
const char* wb_chord_get_name(chord_result_t *result) {
    if (!result) return "Unknown";
    static char buf[32];
    snprintf(buf, sizeof(buf), "%s %s", note_names[result->root], chord_names[result->type]);
    return buf;
}

/* Scale snapping: snap a MIDI note to the nearest note in a scale.
 * scale_bits: 12-bit bitmask of allowed notes (bit 0 = C, bit 1 = C#, etc.) */
static int wb_scale_snap(int midi_note, uint16_t scale_bits) {
    int pc = midi_note % 12;
    if (scale_bits & (1 << pc)) return midi_note;  /* Already in scale */

    /* Search outward for nearest scale note */
    for (int dist = 1; dist <= 6; dist++) {
        int up = (pc + dist) % 12;
        int down = (pc - dist + 12) % 12;
        if (scale_bits & (1 << up)) return midi_note + dist;
        if (scale_bits & (1 << down)) return midi_note - dist;
    }
    return midi_note;  /* No snap found */
}

/* Common scales */
static const uint16_t SCALE_MAJOR       = 0x0AB5;
static const uint16_t SCALE_MINOR       = 0x05AD;
static const uint16_t SCALE_PENTATONIC  = 0x02A9;
static const uint16_t SCALE_BLUES       = 0x06AD;
static const uint16_t SCALE_DORIAN      = 0x0AAD;
static const uint16_t SCALE_MIXOLYDIAN  = 0x0AB5;
static const uint16_t SCALE_CHROMATIC   = 0x0FFF;
