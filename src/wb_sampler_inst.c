/* wb_sampler_inst.c — Sampler Instrument System for YTPMV (R102).
 *
 * The core of YTPMV: load phoneme samples, play them at any pitch
 * via a piano roll, with per-note control over pitch, pan, velocity.
 *
 * This is the FL Studio "Channel Rack + Piano Roll" equivalent:
 * - Each phoneme = one sampler channel
 * - Piano roll notes trigger the sample at different pitches
 * - Per-note: fine pitch (+/- 100 cents), pan, velocity, shift
 * - Beat grid quantization with swing
 * - Video clip trigger: each note maps to a video clip
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ================================================================
 * SAMPLER INSTRUMENT
 * ================================================================
 *
 * A sampler instrument holds one or more audio samples (phonemes).
 * Each sample can be pitch-shifted to any note on the piano roll.
 */

#define MAX_SAMPLES 128
#define MAX_REGIONS 32

/* A sample region: maps a pitch range to a sample */
typedef struct {
    int root_note;         /* MIDI note that plays at original pitch */
    int low_note;          /* lowest note in range */
    int high_note;         /* highest note in range */
    float *audio;          /* sample audio (mono float) */
    int n_frames;          /* number of frames */
    int channels;          /* 1=mono, 2=stereo */
    char name[64];
} wb_sample_region;

/* The sampler instrument */
typedef struct {
    wb_sample_region regions[MAX_REGIONS];
    int n_regions;
    
    /* Global settings */
    float volume;          /* 0..2 */
    float pan;             /* -1..1 */
    int loop;              /* 0=off, 1=loop */
    
    /* Per-note override (from piano roll) */
    float fine_pitch;      /* +/- 100 cents */
    float note_pan;        /* -1..1 */
    float note_velocity;   /* 0..1 */
    float note_shift;      /* timing offset in seconds */
} wb_sampler_inst;

void wb_sampler_init(wb_sampler_inst *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->volume = 1.0f;
    s->pan = 0.0f;
    s->loop = 0;
}

/* Add a sample region */
int wb_sampler_add_region(wb_sampler_inst *s, const float *audio, int n_frames,
                            int channels, int root_note, const char *name) {
    if (!s || !audio || s->n_regions >= MAX_REGIONS) return -1;
    int idx = s->n_regions++;
    wb_sample_region *r = &s->regions[idx];
    r->audio = (float *)malloc(n_frames * sizeof(float));
    if (!r->audio) { s->n_regions--; return -1; }
    /* Mix to mono if stereo */
    for (int i = 0; i < n_frames; i++) {
        if (channels == 2)
            r->audio[i] = (audio[i*2] + audio[i*2+1]) * 0.5f;
        else
            r->audio[i] = audio[i];
    }
    r->n_frames = n_frames;
    r->channels = 1;
    r->root_note = root_note;
    r->low_note = root_note - 12;
    r->high_note = root_note + 12;
    strncpy(r->name, name, 63);
    return idx;
}

/* Find the best region for a given note */
static wb_sample_region* find_region(wb_sampler_inst *s, int note) {
    if (!s) return NULL;
    for (int i = 0; i < s->n_regions; i++) {
        if (note >= s->regions[i].low_note && note <= s->regions[i].high_note)
            return &s->regions[i];
    }
    /* Fallback to first region */
    return s->n_regions > 0 ? &s->regions[0] : NULL;
}

/* Render a note to audio buffer. Handles pitch shifting via resampling. */
int wb_sampler_play_note(wb_sampler_inst *s, int note, float velocity,
                           float *out, int out_frames, int out_channels,
                           float sample_rate) {
    if (!s || !out) return 0;
    
    wb_sample_region *r = find_region(s, note);
    if (!r || !r->audio) return 0;
    
    /* Calculate pitch shift ratio */
    int semitones = note - r->root_note;
    float fine_cents = s->fine_pitch;
    float ratio = powf(2.0f, (semitones + fine_cents / 100.0f) / 12.0f);
    
    /* Apply note velocity and volume */
    float vol = velocity * s->volume;
    
    /* Resample with linear interpolation */
    int frames_written = 0;
    for (int i = 0; i < out_frames; i++) {
        float src_pos = i * ratio;
        int src_idx = (int)src_pos;
        float frac = src_pos - src_idx;
        
        if (src_idx >= r->n_frames) {
            if (s->loop) src_idx = src_idx % r->n_frames;
            else break;
        }
        
        float sample = 0;
        if (src_idx + 1 < r->n_frames) {
            sample = r->audio[src_idx] * (1.0f - frac) + r->audio[src_idx + 1] * frac;
        } else if (src_idx < r->n_frames) {
            sample = r->audio[src_idx];
        }
        
        sample *= vol;
        
        float pan = s->pan + s->note_pan;
        if (pan < -1.0f) pan = -1.0f;
        if (pan > 1.0f) pan = 1.0f;
        float left = (pan <= 0) ? 1.0f : 1.0f - pan;
        float right = (pan >= 0) ? 1.0f : 1.0f + pan;
        
        if (out_channels == 2) {
            out[i*2] += sample * left;
            out[i*2+1] += sample * right;
        } else {
            out[i] += sample;
        }
        frames_written++;
    }
    
    return frames_written;
}

void wb_sampler_free(wb_sampler_inst *s) {
    if (!s) return;
    for (int i = 0; i < s->n_regions; i++)
        free(s->regions[i].audio);
}

/* ================================================================
 * PIANO ROLL SEQUENCER
 * ================================================================
 *
 * A grid-based note sequencer. Notes have:
 * - Position (beat + tick)
 * - Pitch (MIDI note)
 * - Velocity (0..1)
 * - Length (in beats)
 * - Fine pitch (+/- 100 cents)
 * - Pan (-1..1)
 * - Shift (timing offset)
 */

#define MAX_NOTES 4096
#define TICKS_PER_BEAT 480

/* A single note event */
typedef struct {
    int active;
    int tick_position;     /* absolute tick = beat * TICKS_PER_BEAT + tick_in_beat */
    int note;              /* MIDI note 0-127 */
    float velocity;        /* 0..1 */
    int length;            /* in ticks */
    float fine_pitch;      /* +/- 100 cents */
    float pan;             /* -1..1 */
    float shift;           /* timing offset in ticks */
    int clip_index;        /* which video clip to trigger */
} wb_piano_note;

/* The piano roll */
typedef struct {
    wb_piano_note notes[MAX_NOTES];
    int n_notes;
    
    /* Grid settings */
    float bpm;
    int beats_per_bar;
    int bars;
    float swing;           /* 0..0.5 (0 = straight, 0.5 = heavy swing) */
    
    /* Quantize settings */
    int quantize_grid;     /* 1=1/4, 2=1/8, 4=1/16, 8=1/32 */
    int quantize_strength; /* 0..100 (% of way to snap) */
} wb_piano_roll;

void wb_piano_init(wb_piano_roll *pr, float bpm, int beats_per_bar, int bars) {
    if (!pr) return;
    memset(pr, 0, sizeof(*pr));
    pr->bpm = bpm > 0 ? bpm : 120;
    pr->beats_per_bar = beats_per_bar > 0 ? beats_per_bar : 4;
    pr->bars = bars > 0 ? bars : 4;
    pr->quantize_grid = 4; /* 16th notes */
    pr->quantize_strength = 100;
}

/* Add a note */
int wb_piano_add_note(wb_piano_roll *pr, int bar, int beat, int tick_in_beat,
                        int note, float velocity, int length_ticks) {
    if (!pr || pr->n_notes >= MAX_NOTES) return -1;
    
    int tick = (bar * pr->beats_per_bar + beat) * TICKS_PER_BEAT + tick_in_beat;
    
    /* Quantize */
    if (pr->quantize_strength > 0 && pr->quantize_grid > 0) {
        int grid_ticks = TICKS_PER_BEAT / pr->quantize_grid;
        int remainder = tick % grid_ticks;
        int quantized = tick - remainder;
        if (remainder > grid_ticks / 2) quantized += grid_ticks;
        tick = tick + (quantized - tick) * pr->quantize_strength / 100;
    }
    
    /* Apply swing to off-beat 8th notes */
    if (pr->swing > 0) {
        int beat_ticks = tick % TICKS_PER_BEAT;
        if (beat_ticks == TICKS_PER_BEAT / 2) {
            int swing_offset = (int)(pr->swing * TICKS_PER_BEAT / 3.0f);
            tick += swing_offset;
        }
    }
    
    int idx = pr->n_notes++;
    wb_piano_note *n = &pr->notes[idx];
    n->active = 1;
    n->tick_position = tick;
    n->note = note;
    n->velocity = velocity;
    n->length = length_ticks;
    n->fine_pitch = 0;
    n->pan = 0;
    n->shift = 0;
    n->clip_index = -1;
    return idx;
}

/* Find notes at a given tick position */
int wb_piano_get_notes_at(const wb_piano_roll *pr, int tick,
                            int *indices, int max_indices) {
    if (!pr) return 0;
    int count = 0;
    for (int i = 0; i < pr->n_notes && count < max_indices; i++) {
        if (pr->notes[i].active && pr->notes[i].tick_position <= tick &&
            tick < pr->notes[i].tick_position + pr->notes[i].length) {
            indices[count++] = i;
        }
    }
    return count;
}

/* Remove duplicate notes (same pitch, same position) */
int wb_piano_dedupe(wb_piano_roll *pr) {
    if (!pr) return 0;
    int removed = 0;
    for (int i = 0; i < pr->n_notes; i++) {
        if (!pr->notes[i].active) continue;
        for (int j = i + 1; j < pr->n_notes; j++) {
            if (!pr->notes[j].active) continue;
            if (pr->notes[i].note == pr->notes[j].note &&
                pr->notes[i].tick_position == pr->notes[j].tick_position) {
                pr->notes[j].active = 0;
                removed++;
            }
        }
    }
    return removed;
}

/* Transpose all notes by semitones */
void wb_piano_transpose(wb_piano_roll *pr, int semitones) {
    if (!pr) return;
    for (int i = 0; i < pr->n_notes; i++) {
        if (pr->notes[i].active) {
            pr->notes[i].note += semitones;
            if (pr->notes[i].note < 0) pr->notes[i].note = 0;
            if (pr->notes[i].note > 127) pr->notes[i].note = 127;
        }
    }
}

/* Get total length in ticks */
int wb_piano_total_ticks(const wb_piano_roll *pr) {
    if (!pr) return 0;
    return pr->bars * pr->beats_per_bar * TICKS_PER_BEAT;
}

/* ================================================================
 * VIDEO CLIP TRIGGER
 * ================================================================
 *
 * Maps piano roll notes to video clips. Each note triggers a specific
 * video clip that plays for the note's duration.
 */

typedef struct {
    int note;              /* MIDI note that triggers this clip */
    int clip_index;        /* index into video clip database */
    float pitch_shift;     /* additional pitch shift for video speed */
} wb_clip_mapping;

#define MAX_CLIP_MAPPINGS 128

typedef struct {
    wb_clip_mapping mappings[MAX_CLIP_MAPPINGS];
    int n_mappings;
} wb_clip_triggers;

void wb_clip_triggers_init(wb_clip_triggers *ct) {
    if (!ct) return;
    memset(ct, 0, sizeof(*ct));
}

void wb_clip_triggers_map(wb_clip_triggers *ct, int note, int clip_index, float pitch_shift) {
    if (!ct || ct->n_mappings >= MAX_CLIP_MAPPINGS) return;
    int idx = ct->n_mappings++;
    ct->mappings[idx].note = note;
    ct->mappings[idx].clip_index = clip_index;
    ct->mappings[idx].pitch_shift = pitch_shift;
}

/* Find clip for a note */
int wb_clip_triggers_find(wb_clip_triggers *ct, int note, float *pitch_shift) {
    if (!ct) return -1;
    for (int i = 0; i < ct->n_mappings; i++) {
        if (ct->mappings[i].note == note) {
            if (pitch_shift) *pitch_shift = ct->mappings[i].pitch_shift;
            return ct->mappings[i].clip_index;
        }
    }
    return -1;
}

/* ================================================================
 * HARMONY GENERATOR
 * ================================================================
 *
 * Auto-generate harmony tracks from a melody.
 * Supports: thirds, fifths, octaves, custom intervals.
 */

typedef struct {
    int interval_semitones;
    float velocity_scale;
    int active;
} wb_harmony_voice;

#define MAX_HARMONY_VOICES 4

typedef struct {
    wb_harmony_voice voices[MAX_HARMONY_VOICES];
    int n_voices;
} wb_harmony_gen;

void wb_harmony_init(wb_harmony_gen *hg) {
    if (!hg) return;
    memset(hg, 0, sizeof(*hg));
    /* Default: third above */
    hg->voices[0].interval_semitones = 4;
    hg->voices[0].velocity_scale = 0.8f;
    hg->voices[0].active = 1;
    hg->n_voices = 1;
}

void wb_harmony_add_voice(wb_harmony_gen *hg, int semitones, float velocity_scale) {
    if (!hg || hg->n_voices >= MAX_HARMONY_VOICES) return;
    int idx = hg->n_voices++;
    hg->voices[idx].interval_semitones = semitones;
    hg->voices[idx].velocity_scale = velocity_scale;
    hg->voices[idx].active = 1;
}

/* Generate harmony notes from a piano roll */
int wb_harmony_generate(wb_harmony_gen *hg, const wb_piano_roll *source,
                          wb_piano_roll *output) {
    if (!hg || !source || !output) return 0;
    int added = 0;
    for (int i = 0; i < source->n_notes; i++) {
        if (!source->notes[i].active) continue;
        for (int v = 0; v < hg->n_voices; v++) {
            if (!hg->voices[v].active) continue;
            int harm_note = source->notes[i].note + hg->voices[v].interval_semitones;
            if (harm_note > 127) harm_note = 127;
            int bar = source->notes[i].tick_position / (source->beats_per_bar * TICKS_PER_BEAT);
            int remainder = source->notes[i].tick_position % (source->beats_per_bar * TICKS_PER_BEAT);
            int beat = remainder / TICKS_PER_BEAT;
            int tick = remainder % TICKS_PER_BEAT;
            wb_piano_add_note(output, bar, beat, tick, harm_note,
                              source->notes[i].velocity * hg->voices[v].velocity_scale,
                              source->notes[i].length);
            added++;
        }
    }
    return added;
}

/* ================================================================
 * STUTTER RETRIGGER
 * ================================================================
 *
 * Rapid note repeat on beat drops. Creates the classic YTPMV
 * "stutter" effect where a phoneme repeats rapidly.
 */

typedef struct {
    int retrigger_count;   /* number of repeats */
    int retrigger_div;     /* subdivision (1=1/4, 2=1/8, 4=1/16, 8=1/32) */
    float velocity_decay;  /* velocity multiplier per repeat */
} wb_stutter_cfg;

/* Apply stutter to a piano roll note */
int wb_stutter_apply(const wb_stutter_cfg *cfg, const wb_piano_note *source,
                       wb_piano_roll *output, int bar, int beat, int tick) {
    if (!cfg || !source || !output) return 0;
    
    int tick_pos = (bar * output->beats_per_bar + beat) * TICKS_PER_BEAT + tick;
    int sub_ticks = TICKS_PER_BEAT / cfg->retrigger_div;
    int added = 0;
    
    for (int r = 0; r < cfg->retrigger_count; r++) {
        int r_tick = tick_pos + r * sub_ticks;
        float vel = source->velocity * powf(cfg->velocity_decay, r);
        if (vel < 0.05f) break;
        
        /* Direct note add (no quantize) */
        if (output->n_notes >= MAX_NOTES) break;
        int idx = output->n_notes++;
        wb_piano_note *n = &output->notes[idx];
        n->active = 1;
        n->tick_position = r_tick;
        n->note = source->note;
        n->velocity = vel;
        n->length = sub_ticks / 2;
        n->fine_pitch = 0;
        n->pan = 0;
        n->shift = 0;
        n->clip_index = -1;
        added++;
    }
    return added;
}

/* ================================================================
 * BASS DROP GENERATOR
 * ================================================================
 *
 * Detects bass notes (below threshold) and creates a bass drop effect
 * by pitching down an octave and adding a pitch slide.
 */

typedef struct {
    int bass_threshold;    /* notes below this are "bass" */
    int octave_drop;       /* how many octaves to drop */
    float slide_duration;  /* pitch slide duration in beats */
} wb_bass_drop_cfg;

void wb_bass_drop_init(wb_bass_drop_cfg *cfg) {
    if (!cfg) return;
    cfg->bass_threshold = 48; /* C3 */
    cfg->octave_drop = 1;
    cfg->slide_duration = 0.5f;
}

/* Process piano roll: find bass notes and add drop effect */
int wb_bass_drop_process(const wb_bass_drop_cfg *cfg, wb_piano_roll *pr) {
    if (!cfg || !pr) return 0;
    int processed = 0;
    for (int i = 0; i < pr->n_notes; i++) {
        if (!pr->notes[i].active) continue;
        if (pr->notes[i].note <= cfg->bass_threshold) {
            /* Pitch down */
            pr->notes[i].note -= 12 * cfg->octave_drop;
            if (pr->notes[i].note < 0) pr->notes[i].note = 0;
            /* Boost velocity for impact */
            pr->notes[i].velocity = 1.0f;
            processed++;
        }
    }
    return processed;
}
