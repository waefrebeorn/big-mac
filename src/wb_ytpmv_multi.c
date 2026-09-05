/* wb_ytpmv_multi.c — Multi-Character YTPMV Engine (R131).
 *
 * Manages multiple characters, each with their own sample bank.
 * Supports:
 * - Harmony generation (3rd, 5th, octave from lead melody)
 * - Multi-track MIDI (different character per track/channel)
 * - Chord decomposition (each note → different character)
 * - Voice allocation (polyphony management)
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

#define MAX_CHARS 8
#define MAX_SAMPLES_PER_CHAR 16
#define MAX_HARMONY 4
#define MAX_VOICES 16

/* ================================================================
 * CHARACTER
 * ================================================================ */

typedef struct {
    char name[64];
    char source_video[256];
    char source_audio[256];
    
    /* Sample bank */
    struct {
        char name[64];
        float start_time;
        float duration;
        float pitch_hz;
        int midi_note;
        int vowel_class;   /* 0=A, 1=E, 2=I, 3=O, 4=U, -1=unknown */
    } samples[MAX_SAMPLES_PER_CHAR];
    int n_samples;
    
    /* Pitch range */
    float min_pitch_hz;
    float max_pitch_hz;
    int min_midi;
    int max_midi;
    
    /* Quality score (0-1) */
    float quality;
} ytpmv_character;

void ytpmv_character_init(ytpmv_character *ch, const char *name) {
    if (!ch) return;
    memset(ch, 0, sizeof(*ch));
    strncpy(ch->name, name, 63);
    ch->min_pitch_hz = 9999.0f;
    ch->max_pitch_hz = 0.0f;
    ch->min_midi = 127;
    ch->max_midi = 0;
    ch->quality = 0.5f;
}

int ytpmv_character_add_sample(ytpmv_character *ch, const char *name,
                                float start, float dur, float pitch_hz) {
    if (!ch || ch->n_samples >= MAX_SAMPLES_PER_CHAR) return -1;
    int idx = ch->n_samples;
    strncpy(ch->samples[idx].name, name, 63);
    ch->samples[idx].start_time = start;
    ch->samples[idx].duration = dur;
    ch->samples[idx].pitch_hz = pitch_hz;
    ch->samples[idx].midi_note = (int)(69.0f + 12.0f * log2f(pitch_hz / 440.0f) + 0.5f);
    ch->samples[idx].vowel_class = -1; /* Unknown */
    
    /* Update pitch range */
    if (pitch_hz < ch->min_pitch_hz) ch->min_pitch_hz = pitch_hz;
    if (pitch_hz > ch->max_pitch_hz) ch->max_pitch_hz = pitch_hz;
    if (ch->samples[idx].midi_note < ch->min_midi) ch->min_midi = ch->samples[idx].midi_note;
    if (ch->samples[idx].midi_note > ch->max_midi) ch->max_midi = ch->samples[idx].midi_note;
    
    ch->n_samples++;
    return idx;
}

/* Select best sample for a target MIDI note */
int ytpmv_character_select_sample(ytpmv_character *ch, int target_midi, float *out_ratio) {
    if (!ch || ch->n_samples == 0) return -1;
    
    int best = -1;
    float best_dist = 999.0f;
    
    for (int i = 0; i < ch->n_samples; i++) {
        float dist = fabsf((float)(target_midi - ch->samples[i].midi_note));
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    
    if (best >= 0 && out_ratio) {
        float target_freq = 440.0f * powf(2.0f, (target_midi - 69) / 12.0f);
        *out_ratio = target_freq / ch->samples[best].pitch_hz;
    }
    
    return best;
}

/* ================================================================
 * HARMONY GENERATOR
 * ================================================================ */

typedef struct {
    int intervals[MAX_HARMONY];  /* Semitone offsets from root */
    int n_voices;
    float volumes[MAX_HARMONY];  /* Relative volume per voice */
} ytpmv_harmony;

void ytpmv_harmony_init(ytpmv_harmony *h) {
    if (!h) return;
    memset(h, 0, sizeof(*h));
    h->n_voices = 1;
    h->intervals[0] = 0;
    h->volumes[0] = 1.0f;
}

/* Major chord: root, major 3rd, perfect 5th */
void ytpmv_harmony_major(ytpmv_harmony *h) {
    if (!h) return;
    h->n_voices = 3;
    h->intervals[0] = 0;
    h->intervals[1] = 4;
    h->intervals[2] = 7;
    h->volumes[0] = 1.0f;
    h->volumes[1] = 0.7f;
    h->volumes[2] = 0.7f;
}

/* Minor chord: root, minor 3rd, perfect 5th */
void ytpmv_harmony_minor(ytpmv_harmony *h) {
    if (!h) return;
    h->n_voices = 3;
    h->intervals[0] = 0;
    h->intervals[1] = 3;
    h->intervals[2] = 7;
    h->volumes[0] = 1.0f;
    h->volumes[1] = 0.7f;
    h->volumes[2] = 0.7f;
}

/* Power chord: root, 5th, octave */
void ytpmv_harmony_power(ytpmv_harmony *h) {
    if (!h) return;
    h->n_voices = 3;
    h->intervals[0] = 0;
    h->intervals[1] = 7;
    h->intervals[2] = 12;
    h->volumes[0] = 1.0f;
    h->volumes[1] = 0.6f;
    h->volumes[2] = 0.5f;
}

/* Duet: root + 3rd */
void ytpmv_harmony_duet(ytpmv_harmony *h) {
    if (!h) return;
    h->n_voices = 2;
    h->intervals[0] = 0;
    h->intervals[1] = 4;
    h->volumes[0] = 1.0f;
    h->volumes[1] = 0.8f;
}

/* ================================================================
 * MULTI-CHARACTER PROJECT
 * ================================================================ */

typedef struct {
    ytpmv_character characters[MAX_CHARS];
    int n_characters;
    
    /* Track assignment: which character plays which MIDI channel */
    int channel_to_char[16];  /* -1 = unassigned */
    
    /* Harmony settings per character */
    ytpmv_harmony harmony[MAX_CHARS];
    
    /* Global settings */
    float master_volume;
    float reverb_mix;
    float compression_threshold;
} ytpmv_project;

void ytpmv_project_init(ytpmv_project *proj) {
    if (!proj) return;
    memset(proj, 0, sizeof(*proj));
    proj->master_volume = 1.0f;
    proj->reverb_mix = 0.0f;
    proj->compression_threshold = -12.0f;
    for (int i = 0; i < 16; i++)
        proj->channel_to_char[i] = -1;
}

int ytpmv_project_add_character(ytpmv_project *proj, const char *name) {
    if (!proj || proj->n_characters >= MAX_CHARS) return -1;
    ytpmv_character_init(&proj->characters[proj->n_characters], name);
    return proj->n_characters++;
}

ytpmv_character* ytpmv_project_get_character(ytpmv_project *proj, int index) {
    if (!proj || index < 0 || index >= proj->n_characters) return NULL;
    return &proj->characters[index];
}

/* Assign a character to a MIDI channel */
void ytpmv_project_assign_channel(ytpmv_project *proj, int channel, int char_index) {
    if (!proj || channel < 0 || channel >= 16) return;
    proj->channel_to_char[channel] = char_index;
}

/* Auto-assign characters to channels based on pitch range */
void ytpmv_project_auto_assign(ytpmv_project *proj) {
    if (!proj || proj->n_characters == 0) return;
    
    /* Sort characters by pitch range */
    /* Assign lowest character to channel 0 (bass), next to channel 1, etc. */
    for (int i = 0; i < proj->n_characters && i < 16; i++) {
        proj->channel_to_char[i] = i;
    }
}

/* ================================================================
 * VOICE ALLOCATION (polyphony management)
 * ================================================================ */

typedef struct {
    int character_index;
    int sample_index;
    int midi_note;
    float start_time;
    float duration;
    float volume;
    float pitch_ratio;
    int active;
} ytpmv_voice;

typedef struct {
    ytpmv_voice voices[MAX_VOICES];
    int n_active;
} ytpmv_voice_pool;

void ytpmv_voice_pool_init(ytpmv_voice_pool *pool) {
    if (!pool) return;
    memset(pool, 0, sizeof(*pool));
}

/* Allocate a voice for a note */
int ytpmv_voice_allocate(ytpmv_voice_pool *pool, int char_idx, int sample_idx,
                           int midi_note, float start, float dur, float vol, float ratio) {
    if (!pool) return -1;
    
    /* Find free voice */
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!pool->voices[i].active) {
            pool->voices[i].character_index = char_idx;
            pool->voices[i].sample_index = sample_idx;
            pool->voices[i].midi_note = midi_note;
            pool->voices[i].start_time = start;
            pool->voices[i].duration = dur;
            pool->voices[i].volume = vol;
            pool->voices[i].pitch_ratio = ratio;
            pool->voices[i].active = 1;
            pool->n_active++;
            return i;
        }
    }
    return -1; /* No free voices */
}

/* Release voices that have finished */
void ytpmv_voice_pool_update(ytpmv_voice_pool *pool, float current_time) {
    if (!pool) return;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (pool->voices[i].active) {
            float end = pool->voices[i].start_time + pool->voices[i].duration;
            if (current_time >= end) {
                pool->voices[i].active = 0;
                pool->n_active--;
            }
        }
    }
}

/* ================================================================
 * CHORD DECOMPOSITION
 * ================================================================ */

typedef struct {
    int notes[8];
    float start_time;
    float duration;
    int n_notes;
} ytpmv_chord;

/* Decompose a chord into individual notes for multi-character assignment */
int ytpmv_decompose_chord(ytpmv_chord *chord, ytpmv_project *proj,
                           ytpmv_voice *voices, int max_voices) {
    if (!chord || !proj || !voices || chord->n_notes == 0) return 0;
    
    int voice_count = 0;
    
    for (int i = 0; i < chord->n_notes && voice_count < max_voices; i++) {
        int note = chord->notes[i];
        
        /* Select character based on pitch range */
        int best_char = 0;
        float best_dist = 999.0f;
        
        for (int c = 0; c < proj->n_characters; c++) {
            ytpmv_character *ch = &proj->characters[c];
            if (ch->n_samples == 0) continue;
            
            float dist = 0;
            if (note < ch->min_midi) dist = ch->min_midi - note;
            else if (note > ch->max_midi) dist = note - ch->max_midi;
            
            if (dist < best_dist) {
                best_dist = dist;
                best_char = c;
            }
        }
        
        /* Select sample */
        float ratio = 1.0f;
        int sample_idx = ytpmv_character_select_sample(&proj->characters[best_char], note, &ratio);
        
        if (sample_idx >= 0) {
            voices[voice_count].character_index = best_char;
            voices[voice_count].sample_index = sample_idx;
            voices[voice_count].midi_note = note;
            voices[voice_count].start_time = chord->start_time;
            voices[voice_count].duration = chord->duration;
            voices[voice_count].volume = 1.0f / chord->n_notes; /* Distribute volume */
            voices[voice_count].pitch_ratio = ratio;
            voice_count++;
        }
    }
    
    return voice_count;
}

/* ================================================================
 * HARMONY FROM MELODY
 * ================================================================ */

/* Generate harmony notes from a lead melody note */
int ytpmv_generate_harmony(int root_note, ytpmv_harmony *harmony,
                            int *output_notes, float *output_volumes, int max_voices) {
    if (!harmony || !output_notes || !output_volumes) return 0;
    
    int count = 0;
    for (int i = 0; i < harmony->n_voices && i < max_voices; i++) {
        output_notes[count] = root_note + harmony->intervals[i];
        output_volumes[count] = harmony->volumes[i];
        count++;
    }
    
    return count;
}

/* ================================================================
 * QUALITY EVALUATION
 * ================================================================ */

typedef struct {
    float pitch_accuracy;     /* 0-1, how close to target pitch */
    float timing_accuracy;    /* 0-1, how close to beat grid */
    float volume_consistency; /* 0-1, how even the volume is */
    float formant_quality;    /* 0-1, how well formants preserved */
    float overall;            /* 0-1, weighted average */
} ytpmv_quality_score;

/* Evaluate quality of a YTPMV production */
ytpmv_quality_score ytpmv_evaluate(float *pitch_ratios, int n_notes,
                                    float *start_times, float *durations,
                                    float bpm, float sample_rate) {
    ytpmv_quality_score score = {0};
    
    if (n_notes == 0 || !pitch_ratios || !start_times || !durations) return score;
    
    /* Pitch accuracy: how far from 1.0 (no shift needed) */
    float total_shift = 0;
    for (int i = 0; i < n_notes; i++) {
        float shift_semitones = fabsf(12.0f * log2f(pitch_ratios[i]));
        total_shift += shift_semitones;
    }
    float avg_shift = total_shift / n_notes;
    score.pitch_accuracy = fmaxf(0.0f, 1.0f - avg_shift / 7.0f); /* 7 semitones = 0 score */
    
    /* Timing accuracy: how close to beat grid */
    float beat_interval = 60.0f / bpm;
    float total_timing_err = 0;
    for (int i = 0; i < n_notes; i++) {
        float nearest_beat = roundf(start_times[i] / beat_interval) * beat_interval;
        float err = fabsf(start_times[i] - nearest_beat);
        total_timing_err += err;
    }
    float avg_timing_err = total_timing_err / n_notes;
    score.timing_accuracy = fmaxf(0.0f, 1.0f - avg_timing_err / (beat_interval * 0.5f));
    
    /* Volume consistency: coefficient of variation */
    float mean_dur = 0;
    for (int i = 0; i < n_notes; i++) mean_dur += durations[i];
    mean_dur /= n_notes;
    
    float var_dur = 0;
    for (int i = 0; i < n_notes; i++) {
        float diff = durations[i] - mean_dur;
        var_dur += diff * diff;
    }
    var_dur /= n_notes;
    float cv = (mean_dur > 0) ? sqrtf(var_dur) / mean_dur : 1.0f;
    score.volume_consistency = fmaxf(0.0f, 1.0f - cv);
    
    /* Formant quality: based on pitch shift amount */
    /* Smaller shifts = better formant preservation */
    score.formant_quality = fmaxf(0.0f, 1.0f - avg_shift / 12.0f);
    
    /* Overall: weighted average */
    score.overall = score.pitch_accuracy * 0.35f +
                    score.timing_accuracy * 0.30f +
                    score.volume_consistency * 0.15f +
                    score.formant_quality * 0.20f;
    
    return score;
}
