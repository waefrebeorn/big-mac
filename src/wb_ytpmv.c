/* wb_ytpmv.c — YTPMV phoneme pipeline + YTP renderer (R094b).
 *
 * Phoneme extraction, pitch-to-note mapping, beat sequencer, renderer.
 * Pure C11 engine + ffmpeg wrappers for rubberband pitch shift.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ================================================================
 * PHONEME MODEL
 * ================================================================ */

/* Vowel classification by mouth shape (formant frequencies) */
typedef enum {
    PHON_VOWEL_A = 0,  /* "ah" as in father */
    PHON_VOWEL_E,      /* "eh" as in bed */
    PHON_VOWEL_I,      /* "ee" as in see */
    PHON_VOWEL_O,      /* "oh" as in go */
    PHON_VOWEL_U,      /* "oo" as in food */
    PHON_CONSONANT_B,  /* voiced plosive */
    PHON_CONSONANT_D,
    PHON_CONSONANT_F,  /* fricative */
    PHON_CONSONANT_G,
    PHON_CONSONANT_H,
    PHON_CONSONANT_K,
    PHON_CONSONANT_L,  /* liquid */
    PHON_CONSONANT_M,  /* nasal */
    PHON_CONSONANT_N,
    PHON_CONSONANT_P,
    PHON_CONSONANT_R,
    PHON_CONSONANT_S,
    PHON_CONSONANT_T,
    PHON_CONSONANT_V,
    PHON_CONSONANT_W,
    PHON_CONSONANT_Z,
    PHON_SILENCE,
    PHON_UNKNOWN,
    PHON_COUNT
} wb_phoneme_type;

/* A detected phoneme segment */
typedef struct {
    float start_time;      /* seconds */
    float end_time;        /* seconds */
    float duration;        /* seconds */
    float pitch_hz;        /* detected fundamental freq */
    float confidence;      /* 0..1 detection confidence */
    wb_phoneme_type type;  /* classification */
    float energy;          /* RMS energy 0..1 */
    float formant_f1;      /* first formant */
    float formant_f2;      /* second formant */
    int midi_note;         /* mapped MIDI note (0 if unmapped) */
    int velocity;          /* mapped velocity 0..127 */
} wb_phoneme;

/* Phoneme database */
typedef struct {
    wb_phoneme *phonemes;
    int count;
    int capacity;
    float total_duration;
    float bpm;             /* detected tempo */
    float sample_rate;
} wb_phoneme_db;

/* ================================================================
 * PHONEME DATABASE LIFECYCLE
 * ================================================================ */

wb_phoneme_db *wb_phoneme_db_create(int capacity) {
    wb_phoneme_db *db = (wb_phoneme_db *)calloc(1, sizeof(wb_phoneme_db));
    if (!db) return NULL;
    db->phonemes = (wb_phoneme *)calloc(capacity, sizeof(wb_phoneme));
    if (!db->phonemes) { free(db); return NULL; }
    db->capacity = capacity;
    db->count = 0;
    return db;
}

void wb_phoneme_db_free(wb_phoneme_db *db) {
    if (!db) return;
    free(db->phonemes);
    free(db);
}

int wb_phoneme_add(wb_phoneme_db *db, float start, float end,
                    float pitch, float energy, wb_phoneme_type type) {
    if (!db || db->count >= db->capacity) return -1;
    wb_phoneme *p = &db->phonemes[db->count];
    p->start_time = start;
    p->end_time = end;
    p->duration = end - start;
    p->pitch_hz = pitch;
    p->energy = energy;
    p->type = type;
    p->confidence = 0.5f; /* default */
    p->midi_note = 0;
    p->velocity = (int)(energy * 127.0f);
    if (p->velocity > 127) p->velocity = 127;
    if (p->velocity < 1) p->velocity = 1;
    db->count++;
    db->total_duration = end;
    return db->count - 1;
}

/* ================================================================
 * PITCH-TO-NOTE MAPPING
 * ================================================================ */

/* Convert frequency to MIDI note number (A4=440Hz=MIDI 69) */
int freq_to_midi(float freq_hz) {
    if (freq_hz <= 0.0f) return 0;
    int note = (int)(69.0f + 12.0f * log2f(freq_hz / 440.0f) + 0.5f);
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return note;
}

/* Convert MIDI note to frequency */
float midi_to_freq(int midi_note) {
    return 440.0f * powf(2.0f, (float)(midi_note - 69) / 12.0f);
}

/* Get note name from MIDI number */
const char *midi_note_name(int note) {
    static const char *names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    if (note < 0 || note > 127) return "---";
    return names[note % 12];
}

int midi_note_octave(int note) {
    if (note < 0 || note > 127) return -1;
    return note / 12 - 1;
}

/* Calculate pitch shift ratio to reach target MIDI note */
float pitch_shift_ratio(float current_hz, int target_midi) {
    if (current_hz <= 0.0f) return 1.0f;
    float target_hz = midi_to_freq(target_midi);
    return target_hz / current_hz;
}

/* ================================================================
 * SCALE / KEY MAPPING
 * ================================================================ */

/* Scale intervals (semitone offsets from root) */
typedef enum {
    SCALE_MAJOR = 0,
    SCALE_MINOR,
    SCALE_PENTATONIC,
    SCALE_BLUES,
    SCALE_DORIAN,
    SCALE_MIXOLYDIAN,
    SCALE_CHROMATIC,
    SCALE_HARMONIC_MINOR,
    SCALE_MELODIC_MINOR,
    SCALE_WHOLE_TONE,
    SCALE_DIMINISHED,
    SCALE_COUNT
} wb_scale_type;

static const int scale_intervals[][12] = {
    {0,2,4,5,7,9,11},           /* Major */
    {0,2,3,5,7,8,10},           /* Minor */
    {0,2,4,7,9},                /* Pentatonic */
    {0,3,5,6,7,10},             /* Blues */
    {0,2,3,5,7,9,10},           /* Dorian */
    {0,2,4,5,7,9,10},           /* Mixolydian */
    {0,1,2,3,4,5,6,7,8,9,10,11},/* Chromatic */
    {0,2,3,5,7,8,11},           /* Harmonic minor */
    {0,2,3,5,7,9,11},           /* Melodic minor (asc) */
    {0,2,4,6,8,10},             /* Whole tone */
    {0,2,3,5,6,8,9,11},         /* Diminished */
};

static const int scale_sizes[] = {7,7,5,6,7,7,12,7,7,6,8};

/* Quantize a MIDI note to the nearest note in a scale */
int midi_quantize_to_scale(int note, wb_scale_type scale, int root) {
    if (scale < 0 || scale >= SCALE_COUNT) return note;
    int octave = note / 12;
    int pc = note % 12;
    int root_pc = root % 12;

    /* Find closest scale degree */
    int best = pc;
    int best_dist = 99;
    for (int i = 0; i < scale_sizes[scale]; i++) {
        int degree = (root_pc + scale_intervals[scale][i]) % 12;
        int dist = abs(degree - pc);
        if (dist > 6) dist = 12 - dist;
        if (dist < best_dist) {
            best_dist = dist;
            best = degree;
        }
    }
    return octave * 12 + best;
}

/* Check if a MIDI note is in a scale */
int midi_in_scale(int note, wb_scale_type scale, int root) {
    if (scale < 0 || scale >= SCALE_COUNT) return 1;
    int pc = note % 12;
    int root_pc = root % 12;
    for (int i = 0; i < scale_sizes[scale]; i++) {
        if ((root_pc + scale_intervals[scale][i]) % 12 == pc)
            return 1;
    }
    return 0;
}

/* ================================================================
 * BEAT SEQUENCER GRID
 * ================================================================ */

#define WB_SEQ_MAX_STEPS 64
#define WB_SEQ_MAX_TRACKS 16

typedef struct {
    int notes[WB_SEQ_MAX_STEPS];      /* MIDI note (0 = off) */
    int velocities[WB_SEQ_MAX_STEPS]; /* 0..127 */
    int gates[WB_SEQ_MAX_STEPS];      /* gate time 0..100% */
    int enabled[WB_SEQ_MAX_STEPS];    /* step on/off */
    int n_steps;
    int current_step;
    float bpm;
    float swing;          /* 0..0.5 */
    int running;
    void *phoneme_db;     /* reference to source phonemes for pitch */
} wb_seq_track;

typedef struct {
    wb_seq_track tracks[WB_SEQ_MAX_TRACKS];
    int n_tracks;
    float bpm;
    int current_tick;
    int ticks_per_step;   /* usually 4 = 16th notes */
    int running;
} wb_sequencer;

void wb_sequencer_init(wb_sequencer *seq, float bpm, int n_steps) {
    if (!seq) return;
    memset(seq, 0, sizeof(*seq));
    seq->bpm = bpm;
    seq->n_tracks = 1;
    seq->ticks_per_step = 4;
    for (int t = 0; t < WB_SEQ_MAX_TRACKS; t++) {
        seq->tracks[t].n_steps = n_steps < WB_SEQ_MAX_STEPS ? n_steps : WB_SEQ_MAX_STEPS;
        seq->tracks[t].bpm = bpm;
    }
}

void wb_sequencer_set_note(wb_sequencer *seq, int track, int step,
                            int note, int velocity) {
    if (!seq || track < 0 || track >= WB_SEQ_MAX_TRACKS) return;
    if (step < 0 || step >= seq->tracks[track].n_steps) return;
    wb_seq_track *tr = &seq->tracks[track];
    tr->notes[step] = note;
    tr->velocities[step] = velocity > 127 ? 127 : (velocity < 0 ? 0 : velocity);
    tr->enabled[step] = (note > 0) ? 1 : 0;
    tr->gates[step] = 80; /* default 80% gate */
}

/* Tick the sequencer, returns current step for given track */
int wb_sequencer_tick(wb_sequencer *seq, int track) {
    if (!seq || !seq->running || track < 0 || track >= seq->n_tracks) return -1;
    wb_seq_track *tr = &seq->tracks[track];
    int step = tr->current_step;
    tr->current_step = (tr->current_step + 1) % tr->n_steps;
    return step;
}

/* Get current note for a track (-1 if silent) */
int wb_sequencer_current_note(const wb_sequencer *seq, int track) {
    if (!seq || track < 0 || track >= seq->n_tracks) return -1;
    const wb_seq_track *tr = &seq->tracks[track];
    if (!tr->enabled[tr->current_step]) return -1;
    return tr->notes[tr->current_step];
}

void wb_sequencer_start(wb_sequencer *seq) {
    if (seq) { seq->running = 1; seq->current_tick = 0; }
}

void wb_sequencer_stop(wb_sequencer *seq) {
    if (seq) seq->running = 0;
}

/* ================================================================
 * YTPMV RENDERER STATE
 * ================================================================ */

typedef struct {
    wb_phoneme_db *db;
    wb_sequencer *seq;
    float master_bpm;
    wb_scale_type scale;
    int root_note;
    float pitch_shift_max;  /* max semitones shift */
    int formant_preserve;
    int beat_sync_fx;       /* sync visual FX to beat */
} wb_ytpmv_renderer;

wb_ytpmv_renderer *wb_ytpmv_create(wb_phoneme_db *db, float bpm) {
    wb_ytpmv_renderer *r = (wb_ytpmv_renderer *)calloc(1, sizeof(wb_ytpmv_renderer));
    if (!r) return NULL;
    r->db = db;
    r->master_bpm = bpm;
    r->scale = SCALE_CHROMATIC;
    r->root_note = 60; /* C4 */
    r->pitch_shift_max = 24; /* ±2 octaves */
    r->formant_preserve = 1;
    r->beat_sync_fx = 1;
    r->seq = (wb_sequencer *)calloc(1, sizeof(wb_sequencer));
    if (r->seq) wb_sequencer_init(r->seq, bpm, 16);
    return r;
}

void wb_ytpmv_free(wb_ytpmv_renderer *r) {
    if (!r) return;
    free(r->seq);
    free(r);
}
