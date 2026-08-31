/* src/wb_midi_scale.c — MIDI scale quantizer.
 * Snap incoming MIDI notes to a musical scale. Precomputes a 128-entry
 * lookup table for O(1) snapping. For chromatic, all notes pass through.
 * For others, builds from semitone intervals relative to root.
 */
#include "wbus.h"
#include <stdlib.h>
#include <string.h>

/* Internal count of scale types (must match the public enum in wbus.h). */
#define WB_SCALE_COUNT 16

/* Semitone intervals from root for each scale type (within one octave). */
static const int scale_intervals[][12] = {
    /* MAJOR */           {0, 2, 4, 5, 7, 9, 11},
    /* MINOR */            {0, 2, 3, 5, 7, 8, 10},
    /* HARMONIC_MINOR */  {0, 2, 3, 5, 7, 8, 11},
    /* MELODIC_MINOR */   {0, 2, 3, 5, 7, 9, 11},
    /* DORIAN */          {0, 2, 3, 5, 7, 9, 10},
    /* PHRYGIAN */        {0, 1, 3, 5, 7, 8, 10},
    /* LYDIAN */          {0, 2, 4, 6, 7, 9, 11},
    /* MIXOLYDIAN */      {0, 2, 4, 5, 7, 9, 10},
    /* PENTATONIC_MAJOR */{0, 2, 4, 7, 9},
    /* PENTATONIC_MINOR */{0, 3, 5, 7, 10},
    /* BLUES */           {0, 3, 5, 6, 7, 10},
    /* JAPANESE */        {0, 1, 5, 7, 8},
    /* FLAMENCO */        {0, 1, 3, 4, 5, 7, 8, 10},
    /* WHOLE_TONE */      {0, 2, 4, 6, 8, 10},
    /* DIMINISHED */      {0, 2, 3, 5, 6, 8, 9, 11},
    /* CHROMATIC */       {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
};

static const int scale_sizes[] = {
    7,  /* MAJOR */
    7,  /* MINOR */
    7,  /* HARMONIC_MINOR */
    7,  /* MELODIC_MINOR */
    7,  /* DORIAN */
    7,  /* PHRYGIAN */
    7,  /* LYDIAN */
    7,  /* MIXOLYDIAN */
    5,  /* PENTATONIC_MAJOR */
    5,  /* PENTATONIC_MINOR */
    6,  /* BLUES */
    5,  /* JAPANESE */
    8,  /* FLAMENCO */
    6,  /* WHOLE_TONE */
    8,  /* DIMINISHED */
    12, /* CHROMATIC */
};

static const char *scale_names[] = {
    "Major",
    "Minor",
    "Harmonic Minor",
    "Melodic Minor",
    "Dorian",
    "Phrygian",
    "Lydian",
    "Mixolydian",
    "Pentatonic Major",
    "Pentatonic Minor",
    "Blues",
    "Japanese",
    "Flamenco",
    "Whole Tone",
    "Diminished",
    "Chromatic",
};

struct wb_midi_scale {
    int root_note;      /* 0=C, 1=C#, ... 11=B */
    int type;           /* wb_scale_type */
    int snap_up[128];   /* lookup: snap each MIDI note up */
    int snap_down[128]; /* lookup: snap each MIDI note down */
    int in_scale[128];  /* lookup: bool is in scale */
};

static void wb_midi_scale_rebuild(struct wb_midi_scale *sc) {
    int root = sc->root_note % 12;
    int type = sc->type;
    if (type < 0 || type >= WB_SCALE_COUNT) type = WB_SCALE_CHROMATIC;
    sc->type = type;

    /* Build the set of valid pitch classes for this scale. */
    int pc_valid[12] = {0};
    int sz = scale_sizes[type];
    for (int i = 0; i < sz; i++) {
        int pc = (root + scale_intervals[type][i]) % 12;
        pc_valid[pc] = 1;
    }

    /* For chromatic, all notes pass through. */
    if (type == WB_SCALE_CHROMATIC) {
        for (int n = 0; n < 128; n++) {
            sc->snap_up[n] = n;
            sc->snap_down[n] = n;
            sc->in_scale[n] = 1;
        }
        return;
    }

    /* Build lookup tables for all 128 MIDI notes. */
    for (int n = 0; n < 128; n++) {
        int pc = n % 12;
        if (pc_valid[pc]) {
            sc->snap_up[n] = n;
            sc->snap_down[n] = n;
            sc->in_scale[n] = 1;
        } else {
            sc->in_scale[n] = 0;
            /* Find nearest note above (snap up). */
            int up = -1;
            for (int k = 1; k <= 12 && up < 0; k++) {
                int candidate = n + k;
                if (candidate <= 127 && pc_valid[candidate % 12]) {
                    up = candidate;
                }
            }
            sc->snap_up[n] = (up >= 0) ? up : n;

            /* Find nearest note below (snap down). */
            int down = -1;
            for (int k = 1; k <= 12 && down < 0; k++) {
                int candidate = n - k;
                if (candidate >= 0 && pc_valid[candidate % 12]) {
                    down = candidate;
                }
            }
            sc->snap_down[n] = (down >= 0) ? down : n;
        }
    }
}

/* ---- public API ------------------------------------------------------- */

wb_midi_scale *wb_midi_scale_create(void) {
    wb_midi_scale *sc = (wb_midi_scale *)calloc(1, sizeof(wb_midi_scale));
    if (!sc) return NULL;
    sc->root_note = 0;  /* C */
    sc->type = WB_SCALE_MAJOR;
    wb_midi_scale_rebuild(sc);
    return sc;
}

void wb_midi_scale_destroy(void *ptr) {
    free(ptr);
}

void wb_midi_scale_set_root(void *ptr, int root_note) {
    wb_midi_scale *sc = (wb_midi_scale *)ptr;
    if (!sc) return;
    sc->root_note = ((root_note % 12) + 12) % 12;  /* clamp to 0..11 */
    wb_midi_scale_rebuild(sc);
}

void wb_midi_scale_set_type(void *ptr, int type) {
    wb_midi_scale *sc = (wb_midi_scale *)ptr;
    if (!sc) return;
    if (type < 0 || type >= WB_SCALE_COUNT) type = WB_SCALE_CHROMATIC;
    sc->type = type;
    wb_midi_scale_rebuild(sc);
}

int wb_midi_scale_snap(void *ptr, int midi_note) {
    wb_midi_scale *sc = (wb_midi_scale *)ptr;
    if (!sc) return midi_note;
    if (midi_note < 0) return 0;
    if (midi_note > 127) return 127;
    /* snap() = nearest: prefer snap_down if equidistant, else closest. */
    if (sc->in_scale[midi_note]) return midi_note;
    int up = sc->snap_up[midi_note];
    int down = sc->snap_down[midi_note];
    int dist_up = up - midi_note;
    int dist_down = midi_note - down;
    return (dist_up <= dist_down) ? up : down;
}

int wb_midi_scale_snap_up(void *ptr, int midi_note) {
    wb_midi_scale *sc = (wb_midi_scale *)ptr;
    if (!sc) return midi_note;
    if (midi_note < 0) return 0;
    if (midi_note > 127) return 127;
    return sc->snap_up[midi_note];
}

int wb_midi_scale_snap_down(void *ptr, int midi_note) {
    wb_midi_scale *sc = (wb_midi_scale *)ptr;
    if (!sc) return midi_note;
    if (midi_note < 0) return 0;
    if (midi_note > 127) return 127;
    return sc->snap_down[midi_note];
}

int wb_midi_scale_is_in_scale(void *ptr, int midi_note) {
    wb_midi_scale *sc = (wb_midi_scale *)ptr;
    if (!sc) return 0;
    if (midi_note < 0 || midi_note > 127) return 0;
    return sc->in_scale[midi_note];
}

const char *wb_midi_scale_get_name(int type) {
    if (type < 0 || type >= WB_SCALE_COUNT) return NULL;
    return scale_names[type];
}