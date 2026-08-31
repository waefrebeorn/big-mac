/* src/wb_melody_ai.c — AI melody composer.
 *
 * Generate melodies from scale, mood, and rhythm constraints.
 * Uses music theory rules for voice leading, scale-aware generation,
 * and mood-driven rhythmic/melodic character.
 *
 * Moods: 0=happy, 1=sad, 2=energetic, 3=calm
 *
 * Pure C11. Reuses wb_midi_scale for scale-aware snapping.
 */

#include "wbus.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Mood constants */
#define WB_MELODY_AI_MOOD_HAPPY      0
#define WB_MELODY_AI_MOOD_SAD        1
#define WB_MELODY_AI_MOOD_ENERGETIC  2
#define WB_MELODY_AI_MOOD_CALM       3

/* Scale type defaults per mood */
#define SCALE_MAJOR     WB_SCALE_MAJOR
#define SCALE_MINOR     WB_SCALE_MINOR
#define SCALE_PENTA_MIN WB_SCALE_PENTATONIC_MINOR
#define SCALE_BLUES     WB_SCALE_BLUES

struct wb_melody_ai {
    uint32_t    sr;
    uint32_t    seed;
    float       bpm;
    int         mood;
    int         min_note;
    int         max_note;
    void       *scale;  /* wb_midi_scale* */
};

/* ---- xorshift32 PRNG ---- */
static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float wb_ai_randf(uint32_t *state) {
    return (float)(xorshift32(state) & 0xFFFFFF) / (float)0x1000000;
}

static int wb_ai_randi(uint32_t *state, int n) {
    if (n <= 0) return 0;
    return (int)(xorshift32(state) % (uint32_t)n);
}

/* ---- helpers ---- */

/* Duration in beats for rhythm patterns. Returns duration in beats. */
static double wb_ai_pick_duration(struct wb_melody_ai *ai, uint32_t *state) {
    float r = wb_ai_randf(state);
    switch (ai->mood) {
    case WB_MELODY_AI_MOOD_HAPPY:
        /* Balanced: quarters and eighths, occasional sixteenth */
        if (r < 0.35) return 0.25;       /* sixteenth */
        if (r < 0.75) return 0.5;        /* eighth */
        if (r < 0.95) return 1.0;        /* quarter */
        return 2.0;                      /* half */
    case WB_MELODY_AI_MOOD_SAD:
        /* Longer notes, legato feel */
        if (r < 0.1) return 0.5;
        if (r < 0.5) return 1.0;
        if (r < 0.85) return 2.0;
        return 4.0;                      /* whole */
    case WB_MELODY_AI_MOOD_ENERGETIC:
        /* Syncopated: lots of eighths and sixteenths */
        if (r < 0.4) return 0.25;        /* sixteenth */
        if (r < 0.7) return 0.5;         /* eighth */
        if (r < 0.8) return 0.75;        /* dotted-eighth feel (eighth+sixteenth) */
        if (r < 0.95) return 1.0;
        return 0.25;                     /* burst of sixteenths */
    case WB_MELODY_AI_MOOD_CALM:
        /* Slow, sparse: long notes with space */
        if (r < 0.05) return 0.5;
        if (r < 0.3) return 1.0;
        if (r < 0.7) return 2.0;
        return 4.0;
    default:
        return 1.0;
    }
}

/* Pick the next note with voice-leading bias.
 * current: current MIDI note
 * scale: scale quantizer
 * state: PRNG
 * Returns next MIDI note.
 */
static int wb_ai_next_note(struct wb_melody_ai *ai, int current, uint32_t *state) {
    float r = wb_ai_randf(state);
    int next;

    switch (ai->mood) {
    case WB_MELODY_AI_MOOD_HAPPY:
        /* Stepwise motion with occasional leaps up */
        if (r < 0.55) {
            /* Step: up or down by scale step */
            int dir = (wb_ai_randf(state) < 0.6f) ? 1 : -1;
            next = wb_midi_scale_snap_up(ai->scale, current + dir);
            if (next == current) next = wb_midi_scale_snap_down(ai->scale, current - dir);
        } else if (r < 0.80) {
            /* Third up */
            next = wb_midi_scale_snap_up(ai->scale, current + 2);
            if (next == current) next = wb_midi_scale_snap_up(ai->scale, current + 4);
        } else if (r < 0.92) {
            /* Fifth leap up */
            next = wb_midi_scale_snap_up(ai->scale, current + 7);
        } else {
            /* Octave leap up (energetic happy) */
            next = wb_midi_scale_snap_up(ai->scale, current + 12);
        }
        break;

    case WB_MELODY_AI_MOOD_SAD:
        /* Descending patterns, smaller intervals */
        if (r < 0.50) {
            /* Step down */
            next = wb_midi_scale_snap_down(ai->scale, current - 1);
            if (next == current) next = wb_midi_scale_snap_down(ai->scale, current - 2);
        } else if (r < 0.75) {
            /* Second down */
            next = wb_midi_scale_snap_down(ai->scale, current - 2);
        } else if (r < 0.90) {
            /* Third down */
            next = wb_midi_scale_snap_down(ai->scale, current - 4);
        } else {
            /* Resolve to tonic occasionally */
            int tonic = (ai->min_note / 12) * 12 + (ai->min_note % 12);
            /* Find nearest tonic in scale */
            next = wb_midi_scale_snap(ai->scale, tonic);
            if (next < ai->min_note) next = wb_midi_scale_snap_up(ai->scale, tonic + 12);
        }
        break;

    case WB_MELODY_AI_MOOD_ENERGETIC:
        /* Wide range, leaps, staccato feel */
        if (r < 0.30) {
            /* Step */
            int dir = (wb_ai_randf(state) < 0.5f) ? 1 : -1;
            next = wb_midi_scale_snap(ai->scale, current + dir * 2);
        } else if (r < 0.55) {
            /* Third */
            int dir = (wb_ai_randf(state) < 0.5f) ? 1 : -1;
            next = wb_midi_scale_snap(ai->scale, current + dir * 4);
        } else if (r < 0.75) {
            /* Fifth */
            int dir = (wb_ai_randf(state) < 0.5f) ? 1 : -1;
            next = wb_midi_scale_snap(ai->scale, current + dir * 7);
        } else if (r < 0.90) {
            /* Octave leap */
            int dir = (wb_ai_randf(state) < 0.5f) ? 1 : -1;
            next = wb_midi_scale_snap(ai->scale, current + dir * 12);
        } else {
            /* Seventh / wide */
            next = wb_midi_scale_snap(ai->scale, current + wb_ai_randi(state, 14) - 7);
        }
        break;

    case WB_MELODY_AI_MOOD_CALM:
        /* Narrow range, stepwise, legato */
        if (r < 0.65) {
            /* Step */
            int dir = (wb_ai_randf(state) < 0.5f) ? 1 : -1;
            next = wb_midi_scale_snap(ai->scale, current + dir * 2);
        } else if (r < 0.85) {
            /* Second */
            int dir = (wb_ai_randf(state) < 0.5f) ? 1 : -1;
            next = wb_midi_scale_snap(ai->scale, current + dir * 1);
        } else {
            /* Repeat note */
            next = current;
        }
        break;

    default:
        next = current;
        break;
    }

    /* Clamp to range */
    if (next < ai->min_note) next = ai->min_note;
    if (next > ai->max_note) next = ai->max_note;
    if (next < 0) next = 0;
    if (next > 127) next = 127;

    return next;
}

/* Velocity based on mood */
static int wb_ai_pick_velocity(struct wb_melody_ai *ai, uint32_t *state, int is_downbeat) {
    int base, range;
    switch (ai->mood) {
    case WB_MELODY_AI_MOOD_HAPPY:     base = 90; range = 30; break;
    case WB_MELODY_AI_MOOD_SAD:       base = 65; range = 25; break;
    case WB_MELODY_AI_MOOD_ENERGETIC: base = 105; range = 22; break;
    case WB_MELODY_AI_MOOD_CALM:      base = 55; range = 20; break;
    default:                          base = 80; range = 30; break;
    }
    int vel = base + wb_ai_randi(state, range) - range / 2;
    if (is_downbeat) vel += 10;
    if (vel < 20) vel = 20;
    if (vel > 127) vel = 127;
    return vel;
}

/* ---- public API ---- */

void *wb_melody_ai_create(uint32_t sr) {
    struct wb_melody_ai *ai = (struct wb_melody_ai *)calloc(1, sizeof(*ai));
    if (!ai) return NULL;
    ai->sr = sr ? sr : 44100;
    ai->seed = 0xDEADB000;
    ai->bpm = 120.0f;
    ai->mood = WB_MELODY_AI_MOOD_HAPPY;
    ai->min_note = 48;  /* C3 */
    ai->max_note = 84;  /* C6 */
    ai->scale = wb_midi_scale_create();
    if (!ai->scale) { free(ai); return NULL; }
    wb_midi_scale_set_root(ai->scale, 0);
    wb_midi_scale_set_type(ai->scale, WB_SCALE_MAJOR);
    return ai;
}

void wb_melody_ai_destroy(void *ai) {
    struct wb_melody_ai *self = (struct wb_melody_ai *)ai;
    if (!self) return;
    if (self->scale) wb_midi_scale_destroy(self->scale);
    free(self);
}

void wb_melody_ai_set_tempo(void *ai, float bpm) {
    struct wb_melody_ai *self = (struct wb_melody_ai *)ai;
    if (!self) return;
    if (bpm < 20.0f) bpm = 20.0f;
    if (bpm > 400.0f) bpm = 400.0f;
    self->bpm = bpm;
}

void wb_melody_ai_set_mood(void *ai, int mood) {
    struct wb_melody_ai *self = (struct wb_melody_ai *)ai;
    if (!self) return;
    if (mood < 0) mood = 0;
    if (mood > 3) mood = 3;
    self->mood = mood;
}

void wb_melody_ai_set_range(void *ai, int min_note, int max_note) {
    struct wb_melody_ai *self = (struct wb_melody_ai *)ai;
    if (!self) return;
    if (min_note < 0) min_note = 0;
    if (max_note > 127) max_note = 127;
    if (min_note > max_note) { int t = min_note; min_note = max_note; max_note = t; }
    self->min_note = min_note;
    self->max_note = max_note;
}

int wb_melody_ai_compose(void *ai, int scale_root, int scale_type, int mood,
                         int num_bars, int ppq,
                         int *out_notes, int *out_positions,
                         int *out_durations, int *out_velocities,
                         int max_notes) {
    struct wb_melody_ai *self = (struct wb_melody_ai *)ai;
    if (!self || !out_notes || !out_positions || !out_durations || !out_velocities)
        return -1;
    if (max_notes <= 0 || num_bars <= 0) return 0;
    if (ppq <= 0) ppq = 480;

    /* Set mood */
    wb_melody_ai_set_mood(self, mood);

    /* Set scale on the internal scale quantizer */
    wb_midi_scale_set_root(self->scale, scale_root);

    /* Choose scale type based on mood if caller passes a valid type */
    if (scale_type >= 0 && scale_type <= WB_SCALE_CHROMATIC) {
        wb_midi_scale_set_type(self->scale, scale_type);
    } else {
        /* Auto-select based on mood */
        switch (self->mood) {
        case WB_MELODY_AI_MOOD_HAPPY:
            wb_midi_scale_set_type(self->scale, SCALE_MAJOR);
            break;
        case WB_MELODY_AI_MOOD_SAD:
            wb_midi_scale_set_type(self->scale, SCALE_MINOR);
            break;
        case WB_MELODY_AI_MOOD_ENERGETIC:
            wb_midi_scale_set_type(self->scale, SCALE_BLUES);
            break;
        case WB_MELODY_AI_MOOD_CALM:
            wb_midi_scale_set_type(self->scale, SCALE_PENTA_MIN);
            break;
        default:
            wb_midi_scale_set_type(self->scale, SCALE_MAJOR);
            break;
        }
    }

    /* Total beats available */
    double total_beats = (double)num_bars * 4.0;
    uint32_t state = self->seed;

    /* Start note: tonic in the middle of the range */
    int mid_range = (self->min_note + self->max_note) / 2;
    int current = wb_midi_scale_snap(self->scale, mid_range);

    /* Ensure start note is in range */
    if (current < self->min_note) current = self->min_note;
    if (current > self->max_note) current = self->max_note;

    double pos_beats = 0.0;
    int note_idx = 0;

    while (pos_beats < total_beats && note_idx < max_notes) {
        /* Pick duration */
        double dur = wb_ai_pick_duration(self, &state);

        /* Don't exceed total length */
        if (pos_beats + dur > total_beats) {
            dur = total_beats - pos_beats;
            if (dur <= 0) break;
        }

        /* Pick note */
        if (note_idx == 0) {
            current = wb_midi_scale_snap(self->scale, current);
        } else {
            current = wb_ai_next_note(self, current, &state);
        }

        /* Check downbeat for accent */
        int beat_in_bar = (int)pos_beats % 4;
        int is_downbeat = (beat_in_bar == 0);

        /* Velocity */
        int vel = wb_ai_pick_velocity(self, &state, is_downbeat);

        /* For calm, add space: sometimes skip (rest) */
        int is_rest = 0;
        if (self->mood == WB_MELODY_AI_MOOD_CALM && wb_ai_randf(&state) < 0.25) {
            is_rest = 1;
        }

        /* Output */
        out_notes[note_idx] = current;
        out_positions[note_idx] = (int)(pos_beats * ppq);
        out_durations[note_idx] = (int)(dur * ppq);
        out_velocities[note_idx] = is_rest ? 0 : vel;

        note_idx++;
        pos_beats += dur;

        /* Tempo affects density: faster tempo = more notes (shorter durations) */
        if (self->bpm > 180.0f && note_idx < max_notes) {
            /* Insert an extra short note for high tempo */
            double extra_dur = (self->mood == WB_MELODY_AI_MOOD_ENERGETIC) ? 0.25 : 0.5;
            if (pos_beats + extra_dur <= total_beats) {
                current = wb_ai_next_note(self, current, &state);
                out_notes[note_idx] = current;
                out_positions[note_idx] = (int)(pos_beats * ppq);
                out_durations[note_idx] = (int)(extra_dur * ppq);
                out_velocities[note_idx] = wb_ai_pick_velocity(self, &state, 0) - 10;
                note_idx++;
                pos_beats += extra_dur;
            }
        }
    }

    /* Resolve last note to tonic for voice-leading closure */
    if (note_idx > 0 && self->mood != WB_MELODY_AI_MOOD_ENERGETIC) {
        int tonic = wb_midi_scale_snap(self->scale,
            ((self->min_note + self->max_note) / 24) * 12 + scale_root);
        if (tonic < self->min_note) tonic += 12;
        if (tonic <= self->max_note && tonic >= self->min_note) {
            out_notes[note_idx - 1] = tonic;
        }
    }

    self->seed = state;
    return note_idx;
}