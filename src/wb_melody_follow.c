/* wb_melody_follow.c — Melody-Following Pitch Mapper (R112).
 *
 * The key technique for YTPMV that sounds "in tune" with the target song:
 * instead of snapping phonemes to the nearest scale note, we map them
 * to follow the actual melody contour of the target song.
 *
 * Workflow:
 * 1. Provide target melody (MIDI notes or pitch contour array)
 * 2. Detect phonemes from source audio
 * 3. For each phoneme, look up the target melody pitch at that time
 * 4. Pitch-shift the phoneme to match the target
 * 5. Render the result
 *
 * This makes the character "sing" the actual melody rather than just
 * hitting random notes in a scale.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ================================================================
 * MELODY CONTAINER
 * ================================================================
 *
 * Represents a target melody as a series of (time, midi_note) events.
 * Can be loaded from MIDI or manually specified.
 */

#define MAX_MELODY_NOTES 1024

typedef struct {
    float start_time;      /* seconds */
    float duration;        /* seconds */
    int midi_note;         /* 0-127, 0 = rest */
    float velocity;        /* 0-1 */
} wb_melody_event;

typedef struct {
    wb_melody_event events[MAX_MELODY_NOTES];
    int n_events;
    float total_duration;
    float bpm;
} wb_melody;

void wb_melody_init(wb_melody *m, float bpm, float duration) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->bpm = bpm > 0 ? bpm : 120.0f;
    m->total_duration = duration;
}

int wb_melody_add_note(wb_melody *m, float start, float dur, int midi, float vel) {
    if (!m || m->n_events >= MAX_MELODY_NOTES) return -1;
    wb_melody_event *e = &m->events[m->n_events];
    e->start_time = start;
    e->duration = dur;
    e->midi_note = midi;
    e->velocity = vel > 1.0f ? 1.0f : (vel < 0 ? 0 : vel);
    m->n_events++;
    return 0;
}

/* Get the MIDI note at a given time (0 = rest/silence) */
int wb_melody_note_at(const wb_melody *m, float time_sec) {
    if (!m) return 0;
    for (int i = 0; i < m->n_events; i++) {
        if (time_sec >= m->events[i].start_time &&
            time_sec < m->events[i].start_time + m->events[i].duration) {
            return m->events[i].midi_note;
        }
    }
    return 0; /* rest */
}

/* Get the frequency (Hz) at a given time */
float wb_melody_freq_at(const wb_melody *m, float time_sec) {
    if (!m) return 0;
    int midi = wb_melody_note_at(m, time_sec);
    if (midi <= 0) return 0;
    return 440.0f * powf(2.0f, (midi - 69) / 12.0f);
}

/* ================================================================
 * MELODY-FOLLOWING PITCH MAPPER
 * ================================================================
 *
 * Maps source phonemes to follow a target melody.
 */

typedef struct {
    wb_melody target;          /* target melody to follow */
    float *source_audio;       /* source audio buffer */
    int source_frames;
    int source_channels;
    float sample_rate;
    
    /* Phoneme assignments */
    float *phoneme_starts;     /* start times in seconds */
    float *phoneme_durations;  /* durations in seconds */
    int *phoneme_target_midi;  /* assigned target MIDI note */
    float *phoneme_pitch_ratio;/* source_pitch / target_freq */
    int n_phonemes;
    
    /* Output */
    float *output_audio;
    int output_frames;
} wb_melody_mapper;

void wb_mapper_init(wb_melody_mapper *mm, float sample_rate) {
    if (!mm) return;
    memset(mm, 0, sizeof(*mm));
    mm->sample_rate = sample_rate > 0 ? sample_rate : 44100.0f;
}

/* Assign phonemes to melody notes based on timing */
void wb_mapper_assign(wb_melody_mapper *mm,
                       const float *start_times, const float *durations,
                       int n_phonemes) {
    if (!mm || !start_times || !durations) return;
    
    int n = n_phonemes;
    if (n > 1024) n = 1024;
    mm->n_phonemes = n;
    
    mm->phoneme_starts = (float *)calloc(n, sizeof(float));
    mm->phoneme_durations = (float *)calloc(n, sizeof(float));
    mm->phoneme_target_midi = (int *)calloc(n, sizeof(int));
    mm->phoneme_pitch_ratio = (float *)calloc(n, sizeof(float));
    
    for (int i = 0; i < n; i++) {
        mm->phoneme_starts[i] = start_times[i];
        mm->phoneme_durations[i] = durations[i];
        
        /* Look up target melody note at this phoneme's time */
        float mid_time = start_times[i] + durations[i] * 0.5f;
        int target_midi = wb_melody_note_at(&mm->target, mid_time);
        mm->phoneme_target_midi[i] = target_midi;
        
        /* Calculate pitch ratio: target_freq / source_freq */
        float target_freq = 440.0f * powf(2.0f, (target_midi - 69) / 12.0f);
        /* Source pitch will be detected during render */
        mm->phoneme_pitch_ratio[i] = target_freq; /* store target freq for now */
    }
}

/* Render: pitch-shift each phoneme to its target melody note */
int wb_mapper_render(wb_melody_mapper *mm, float *output, int out_frames) {
    if (!mm || !output || !mm->source_audio || mm->n_phonemes == 0) return 0;
    
    /* Clear output */
    memset(output, 0, out_frames * sizeof(float));
    
    int sr = (int)mm->sample_rate;
    int rendered = 0;
    
    for (int i = 0; i < mm->n_phonemes; i++) {
        if (mm->phoneme_target_midi[i] <= 0) continue; /* rest */
        
        int start_sample = (int)(mm->phoneme_starts[i] * sr);
        int dur_samples = (int)(mm->phoneme_durations[i] * sr);
        int target_midi = mm->phoneme_target_midi[i];
        float target_freq = mm->phoneme_pitch_ratio[i];
        
        /* Detect source pitch for this phoneme (simple autocorrelation) */
        int analysis_len = dur_samples;
        if (analysis_len > 2048) analysis_len = 2048;
        if (start_sample + analysis_len > mm->source_frames)
            analysis_len = mm->source_frames - start_sample;
        if (analysis_len < 64) continue;
        
        /* Simple pitch detection: zero-crossing rate */
        int crossings = 0;
        for (int j = start_sample + 1; j < start_sample + analysis_len; j++) {
            int idx = j * mm->source_channels;
            int prev_idx = (j-1) * mm->source_channels;
            if ((mm->source_audio[idx] >= 0) != (mm->source_audio[prev_idx] >= 0))
                crossings++;
        }
        float source_freq = (float)crossings * sr / (2.0f * analysis_len);
        if (source_freq < 50.0f) source_freq = 120.0f; /* fallback */
        if (source_freq > 1000.0f) source_freq = 440.0f;
        
        /* Pitch shift ratio */
        float ratio = target_freq / source_freq;
        if (ratio > 4.0f) ratio = 4.0f;   /* limit to 2 octaves up */
        if (ratio < 0.25f) ratio = 0.25f; /* limit to 2 octaves down */
        
        /* Resample: read source at rate 1/ratio to shift pitch */
        for (int j = 0; j < dur_samples && (start_sample + j) < out_frames; j++) {
            float src_pos = j * ratio;
            int src_idx = (int)src_pos;
            
            if (src_idx >= dur_samples) break;
            
            /* Linear interpolation */
            float frac = src_pos - src_idx;
            int s0 = (start_sample + src_idx) * mm->source_channels;
            int s1 = s0 + mm->source_channels;
            
            if (s1 / mm->source_channels >= mm->source_frames) break;
            
            float sample = mm->source_audio[s0] * (1 - frac) +
                          mm->source_audio[s1] * frac;
            
            /* Apply simple envelope to avoid clicks at boundaries */
            float envelope = 1.0f;
            int fade_len = sr / 100; /* 10ms fade */
            if (j < fade_len) envelope = (float)j / fade_len;
            if (j > dur_samples - fade_len) envelope = (float)(dur_samples - j) / fade_len;
            
            output[start_sample + j] += sample * envelope;
            if (start_sample + j > rendered) rendered = start_sample + j;
        }
    }
    
    return rendered;
}

void wb_mapper_free(wb_melody_mapper *mm) {
    if (!mm) return;
    free(mm->phoneme_starts);
    free(mm->phoneme_durations);
    free(mm->phoneme_target_midi);
    free(mm->phoneme_pitch_ratio);
    free(mm->output_audio);
    memset(mm, 0, sizeof(*mm));
}

/* ================================================================
 * HELPER: Build melody from simple patterns
 * ================================================================
 */

/* Build a C major scale melody (for testing) */
void wb_melody_build_c_major(wb_melody *m, float bpm, int bars) {
    if (!m) return;
    wb_melody_init(m, bpm, bars * 4 * 60.0f / bpm);
    
    /* C major scale MIDI notes: C4=60, D4=62, E4=64, F4=65, G4=67, A4=69, B4=71, C5=72 */
    int scale[] = {60, 62, 64, 65, 67, 69, 71, 72, 72, 71, 69, 67, 65, 64, 62, 60};
    int n_notes = sizeof(scale) / sizeof(scale[0]);
    
    float beat_dur = 60.0f / bpm;
    float time = 0;
    
    for (int bar = 0; bar < bars; bar++) {
        for (int i = 0; i < n_notes && m->n_events < MAX_MELODY_NOTES; i++) {
            wb_melody_add_note(m, time, beat_dur * 0.5f, scale[i], 0.9f);
            time += beat_dur * 0.5f;
        }
    }
    
    m->total_duration = time;
}

/* Build a simple melody from note array */
void wb_melody_from_notes(wb_melody *m, const int *midi_notes, const float *durations,
                            int n_notes, float bpm) {
    if (!m || !midi_notes || !durations) return;
    wb_melody_init(m, bpm, 0);
    
    float time = 0;
    for (int i = 0; i < n_notes && m->n_events < MAX_MELODY_NOTES; i++) {
        wb_melody_add_note(m, time, durations[i], midi_notes[i], 0.9f);
        time += durations[i];
    }
    m->total_duration = time;
}
