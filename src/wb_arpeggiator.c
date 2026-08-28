/* wb_arpeggiator.c — MIDI arpeggiator.
 *
 * R077: Pattern-based note generation from held chords.
 *
 * Patterns:
 *   0: Up
 *   1: Down
 *   2: Up/Down
 *   3: Random
 *   4: As played
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_HELD_NOTES 16

typedef enum {
    ARP_UP = 0,
    ARP_DOWN,
    ARP_UPDOWN,
    ARP_RANDOM,
    ARP_AS_PLAYED
} arp_pattern_t;

typedef struct {
    uint32_t sr;
    arp_pattern_t pattern;
    int      bpm;
    int      note_division;  /* 1=quarter, 2=eighth, 4=sixteenth, 3=triplet */
    float    gate_percent;   /* Note on percentage (0.0-1.0) */
    int      octave_range;   /* How many octaves to span (1-4) */

    /* State */
    int      held_notes[MAX_HELD_NOTES];
    int      num_held;
    int      current_index;
    int      current_octave;
    int      direction;      /* 1=up, -1=down */
    float    time_accum;
    int      note_active;
    int      active_note;
    unsigned int rng;
} wb_arp_inst;

void *wb_arp_create(uint32_t sr) {
    wb_arp_inst *arp = (wb_arp_inst *)calloc(1, sizeof(*arp));
    if (!arp) return NULL;
    arp->sr = sr;
    arp->pattern = ARP_UP;
    arp->bpm = 120;
    arp->note_division = 4;  /* 16th notes */
    arp->gate_percent = 0.8f;
    arp->octave_range = 1;
    arp->current_index = 0;
    arp->current_octave = 0;
    arp->direction = 1;
    arp->note_active = 0;
    arp->active_note = -1;
    arp->rng = 0xDEADBEEF;
    return arp;
}

void wb_arp_destroy(void *inst) { free(inst); }

void wb_arp_set(void *inst, int param, float v) {
    wb_arp_inst *arp = (wb_arp_inst *)inst;
    if (!arp) return;
    switch (param) {
    case 0: arp->pattern = (arp_pattern_t)(int)v; break;
    case 1: arp->bpm = (int)v > 20 ? (int)v : 20; break;
    case 2: arp->note_division = (int)v > 0 ? (int)v : 1; break;
    case 3: arp->gate_percent = v < 0.1f ? 0.1f : (v > 1.0f ? 1.0f : v); break;
    case 4: arp->octave_range = (int)v > 0 ? ((int)v > 4 ? 4 : (int)v) : 1; break;
    default: break;
    }
}

/* Add a held note. */
void wb_arp_note_on(void *inst, int note) {
    wb_arp_inst *arp = (wb_arp_inst *)inst;
    if (!arp || arp->num_held >= MAX_HELD_NOTES) return;

    /* Check if already held */
    for (int i = 0; i < arp->num_held; i++) {
        if (arp->held_notes[i] == note) return;
    }

    /* Insert sorted by pitch */
    int pos = arp->num_held;
    for (int i = 0; i < arp->num_held; i++) {
        if (note < arp->held_notes[i]) {
            pos = i;
            break;
        }
    }
    /* Shift and insert */
    for (int i = arp->num_held; i > pos; i--) {
        arp->held_notes[i] = arp->held_notes[i-1];
    }
    arp->held_notes[pos] = note;
    arp->num_held++;
}

/* Remove a held note. */
void wb_arp_note_off(void *inst, int note) {
    wb_arp_inst *arp = (wb_arp_inst *)inst;
    if (!arp) return;

    for (int i = 0; i < arp->num_held; i++) {
        if (arp->held_notes[i] == note) {
            /* Shift down */
            for (int j = i; j < arp->num_held - 1; j++) {
                arp->held_notes[j] = arp->held_notes[j+1];
            }
            arp->num_held--;
            return;
        }
    }
}

/* Get the next arpeggiated note.
 * Returns MIDI note number, or -1 if no note should play.
 * time_sec: current time in seconds */
int wb_arp_get_next_note(void *inst, float time_sec) {
    wb_arp_inst *arp = (wb_arp_inst *)inst;
    if (!arp || arp->num_held == 0) return -1;

    /* Calculate step duration */
    float beat_duration = 60.0f / (float)arp->bpm;
    float step_duration = beat_duration / (float)arp->note_division;

    /* Check if it's time for a new note */
    int step = (int)(time_sec / step_duration);
    if (step == (int)((time_sec - 0.001f) / step_duration)) {
        return arp->active_note;  /* Same step, return current note */
    }

    /* Advance to next note */
    int total_notes = arp->num_held * arp->octave_range;
    int note_idx = 0;
    int octave_offset = 0;

    switch (arp->pattern) {
    case ARP_UP:
        note_idx = arp->current_index % arp->num_held;
        octave_offset = (arp->current_index / arp->num_held) * 12;
        arp->current_index = (arp->current_index + 1) % total_notes;
        break;

    case ARP_DOWN:
        note_idx = (arp->num_held - 1) - (arp->current_index % arp->num_held);
        octave_offset = (arp->current_index / arp->num_held) * 12;
        arp->current_index = (arp->current_index + 1) % total_notes;
        break;

    case ARP_UPDOWN:
        if (arp->direction > 0) {
            note_idx = arp->current_index;
            if (note_idx >= arp->num_held - 1) {
                arp->direction = -1;
            } else {
                arp->current_index++;
            }
        } else {
            note_idx = arp->current_index;
            if (note_idx <= 0) {
                arp->direction = 1;
                arp->current_index++;
            } else {
                arp->current_index--;
            }
        }
        break;

    case ARP_RANDOM:
        arp->rng = arp->rng * 1103515245u + 12345u;
        note_idx = arp->rng % arp->num_held;
        octave_offset = (arp->rng >> 16) % arp->octave_range * 12;
        break;

    case ARP_AS_PLAYED:
        note_idx = arp->current_index % arp->num_held;
        arp->current_index = (arp->current_index + 1) % arp->num_held;
        break;
    }

    int note = arp->held_notes[note_idx] + octave_offset;
    arp->active_note = note;
    return note;
}
