#define MAX_VIDEO_CLIPS 32
/* wb_ytpmv_engine.c — YTPMV Production Engine (R124).
 *
 * The core engine that ties together:
 * - Sample management (multi-sample, pitch-matched)
 * - MIDI-driven pitch mapping
 * - Volume envelope generation
 * - Video segment timing calculation
 * - Timeline composition
 *
 * This is the engine behind the YTPMV production pipeline.
 * It generates the data that ffmpeg CLI tools consume.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wbus/wbus_compositor.h"

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ================================================================
 * SAMPLE MANAGEMENT
 * ================================================================ */

#define MAX_SAMPLES 16
#define MAX_NOTES 256

void wb_ytpmv_arr_init(wb_ytpmv_arrangement *arr) {
    if (!arr) return;
    memset(arr, 0, sizeof(*arr));
    arr->bpm = 120.0f;
}

void wb_ytpmv_bank_init(wb_ytpmv_bank *bank) {
    if (!bank) return;
    memset(bank, 0, sizeof(*bank));
    bank->sample_rate = 44100.0f;
}

int wb_ytpmv_bank_add(wb_ytpmv_bank *bank, const char *name,
                       float start, float dur, float pitch_hz) {
    if (!bank || bank->n_samples >= MAX_SAMPLES) return -1;
    wb_ytpmv_sample *s = &bank->samples[bank->n_samples];
    strncpy(s->name, name, 63);
    s->start_time = start;
    s->duration = dur;
    s->pitch_hz = pitch_hz;
    s->midi_note = (int)(69.0f + 12.0f * log2f(pitch_hz / 440.0f) + 0.5f);
    s->usable_range_semitones = 5.0f;
    bank->n_samples++;
    return bank->n_samples - 1;
}

int wb_ytpmv_bank_select(wb_ytpmv_bank *bank, int target_midi, float *out_ratio) {
    if (!bank || bank->n_samples == 0) return -1;
    int best = -1;
    float best_dist = 999.0f;
    for (int i = 0; i < bank->n_samples; i++) {
        float dist = fabsf((float)(target_midi - bank->samples[i].midi_note));
        if (dist < best_dist) { best_dist = dist; best = i; }
    }
    if (best >= 0 && out_ratio) {
        float target_freq = 440.0f * powf(2.0f, (target_midi - 69) / 12.0f);
        *out_ratio = target_freq / bank->samples[best].pitch_hz;
    }
    return best;
}


void wb_ytpmv_engine_init(wb_ytpmv_engine *proj) {
    if (!proj) return;
    memset(proj, 0, sizeof(*proj));
    proj->sample_rate = 44100.0f;
    proj->audio_channels = 1;
    proj->video_width = 854.0f;
    proj->video_height = 480.0f;
    proj->video_fps = 24.0f;
    proj->min_clip_duration = 0.5f;
    proj->fade_duration = 0.005f;
    proj->max_pitch_shift = 5.0f;
    wb_ytpmv_arr_init(&proj->arrangement);
    wb_ytpmv_bank_init(&proj->sample_bank);
}

/* Process MIDI notes: assign samples, compute pitch ratios */
void wb_ytpmv_engine_process(wb_ytpmv_engine *proj) {
    if (!proj) return;
    
    wb_ytpmv_arrangement *arr = &proj->arrangement;
    wb_ytpmv_bank *bank = &proj->sample_bank;
    
    for (int i = 0; i < arr->n_notes; i++) {
        wb_ytpmv_note *note = &arr->notes[i];
        
        /* Select best sample */
        float ratio = 1.0f;
        int sample_idx = wb_ytpmv_bank_select(bank, note->midi_note, &ratio);
        note->sample_index = sample_idx;
        note->pitch_ratio = ratio;
        
        /* Compute video speed */
        float note_dur = note->duration;
        if (note_dur < proj->min_clip_duration)
            note_dur = proj->min_clip_duration;
        
        /* Cycle through video clips */
        note->video_clip_index = i % MAX_VIDEO_CLIPS;
        
        /* Video speed = source_duration / note_duration */
        float src_dur = 0.4f; /* Default source clip duration */
        note->video_speed = src_dur / note_dur;
        
        /* Clamp speed */
        if (note->video_speed > 4.0f) note->video_speed = 4.0f;
        if (note->video_speed < 0.25f) note->video_speed = 0.25f;
    }
}

/* Calculate total output duration */
float wb_ytpmv_engine_duration(wb_ytpmv_engine *proj) {
    if (!proj) return 0;
    
    float total = 0;
    for (int i = 0; i < proj->arrangement.n_notes; i++) {
        wb_ytpmv_note *note = &proj->arrangement.notes[i];
        float end = note->start_time + note->duration;
        if (end > total) total = end;
    }
    proj->arrangement.total_duration = total;
    return total;
}

/* Generate volume envelope for a note */
void wb_ytpmv_generate_envelope(float *buffer, int n_frames, float sample_rate,
                                  float fade_dur) {
    if (!buffer || n_frames <= 0) return;
    
    int fade_frames = (int)(fade_dur * sample_rate);
    if (fade_frames < 1) fade_frames = 1;
    if (fade_frames > n_frames / 2) fade_frames = n_frames / 2;
    
    for (int i = 0; i < n_frames; i++) {
        float env = 1.0f;
        if (i < fade_frames) {
            env = (float)i / fade_frames; /* Fade in */
        } else if (i > n_frames - fade_frames) {
            env = (float)(n_frames - i) / fade_frames; /* Fade out */
        }
        buffer[i] *= env;
    }
}

/* Pick a clean vowel segment from audio using energy + ZCR */

int wb_ytpmv_find_vowels(const float *audio, int n_frames, int n_channels,
                           float sample_rate, wb_ytpmv_vowel_segment *segments,
                           int max_segments) {
    if (!audio || !segments || n_frames <= 0) return 0;
    
    int window_ms = 20;
    int window_size = (int)(sample_rate * window_ms / 1000.0f);
    if (window_size < 64) window_size = 64;
    int hop = window_size / 2;
    
    /* First pass: find candidate segments */
    typedef struct {
        float time;
        float energy;
        float zcr;
        float pitch;
    } candidate_t;
    
    candidate_t *candidates = (candidate_t *)calloc(n_frames / hop, sizeof(candidate_t));
    int n_candidates = 0;
    
    for (int i = 0; i < n_frames - window_size; i += hop) {
        double energy = 0;
        int crossings = 0;
        
        for (int j = 0; j < window_size; j++) {
            float s = 0;
            for (int c = 0; c < n_channels; c++)
                s += audio[(i + j) * n_channels + c];
            s /= n_channels;
            energy += s * s;
            
            if (j > 0) {
                float sp = 0;
                for (int c = 0; c < n_channels; c++)
                    sp += audio[(i + j - 1) * n_channels + c];
                sp /= n_channels;
                if ((s >= 0) != (sp >= 0)) crossings++;
            }
        }
        
        energy /= window_size;
        float zcr = (float)crossings / window_size;
        float pitch = sample_rate * zcr / 2.0f;
        
        /* Vowel criteria */
        if (energy > 1e6f && 0.03f < zcr && zcr < 0.35f && pitch > 80.0f && pitch < 1000.0f) {
            candidates[n_candidates].time = (float)i / sample_rate;
            candidates[n_candidates].energy = (float)energy;
            candidates[n_candidates].zcr = zcr;
            candidates[n_candidates].pitch = pitch;
            n_candidates++;
        }
    }
    
    /* Merge adjacent candidates */
    int n_segments = 0;
    int i = 0;
    while (i < n_candidates && n_segments < max_segments) {
        float start = candidates[i].time;
        float end = start + window_ms / 1000.0f;
        double total_pitch = candidates[i].pitch;
        double total_energy = candidates[i].energy;
        int count = 1;
        
        int j = i + 1;
        while (j < n_candidates && candidates[j].time - end < 0.05f) {
            end = candidates[j].time + window_ms / 1000.0f;
            total_pitch += candidates[j].pitch;
            total_energy += candidates[j].energy;
            count++;
            j++;
        }
        
        float dur = end - start;
        if (dur > 0.05f) {
            float avg_pitch = (float)(total_pitch / count);
            float avg_energy = (float)(total_energy / count);
            int midi = (int)(69.0f + 12.0f * log2f(avg_pitch / 440.0f) + 0.5f);
            
            segments[n_segments].start_time = start;
            segments[n_segments].duration = dur;
            segments[n_segments].pitch_hz = avg_pitch;
            segments[n_segments].midi_note = midi;
            
            /* Quality score: higher energy = better, moderate ZCR = better */
            float zcr_norm = 1.0f - fabsf(0.15f - candidates[i].zcr) / 0.15f;
            float energy_norm = fminf(avg_energy / 1e7f, 1.0f);
            segments[n_segments].quality = zcr_norm * energy_norm;
            
            n_segments++;
        }
        
        i = j;
    }
    
    free(candidates);
    return n_segments;
}

/* Select best N samples covering different pitch ranges */
int wb_ytpmv_select_samples(wb_ytpmv_vowel_segment *segments, int n_segments,
                              wb_ytpmv_sample *samples, int n_samples) {
    if (!segments || !samples || n_segments == 0 || n_samples == 0) return 0;
    
    /* Sort by pitch */
    for (int i = 0; i < n_segments - 1; i++) {
        for (int j = i + 1; j < n_segments; j++) {
            if (segments[j].pitch_hz < segments[i].pitch_hz) {
                wb_ytpmv_vowel_segment tmp = segments[i];
                segments[i] = segments[j];
                segments[j] = tmp;
            }
        }
    }
    
    /* Pick lowest, middle, highest quality samples */
    int count = 0;
    
    /* Lowest pitch sample */
    float best_quality = 0;
    int best_idx = 0;
    for (int i = 0; i < n_segments && segments[i].midi_note < 55; i++) {
        if (segments[i].quality > best_quality) {
            best_quality = segments[i].quality;
            best_idx = i;
        }
    }
    samples[count].start_time = segments[best_idx].start_time;
    samples[count].duration = segments[best_idx].duration;
    samples[count].pitch_hz = segments[best_idx].pitch_hz;
    samples[count].midi_note = segments[best_idx].midi_note;
    samples[count].usable_range_semitones = 5.0f;
    snprintf(samples[count].name, 63, "low_%d", samples[count].midi_note);
    count++;
    
    /* Middle pitch sample */
    best_quality = 0;
    best_idx = n_segments / 2;
    int mid_low = n_segments / 3;
    int mid_high = 2 * n_segments / 3;
    for (int i = mid_low; i <= mid_high && i < n_segments; i++) {
        if (segments[i].quality > best_quality) {
            best_quality = segments[i].quality;
            best_idx = i;
        }
    }
    if (count < n_samples) {
        samples[count].start_time = segments[best_idx].start_time;
        samples[count].duration = segments[best_idx].duration;
        samples[count].pitch_hz = segments[best_idx].pitch_hz;
        samples[count].midi_note = segments[best_idx].midi_note;
        samples[count].usable_range_semitones = 5.0f;
        snprintf(samples[count].name, 63, "mid_%d", samples[count].midi_note);
        count++;
    }
    
    /* Highest pitch sample */
    best_quality = 0;
    best_idx = n_segments - 1;
    for (int i = 0; i < n_segments && segments[i].midi_note > 65; i++) {
        if (segments[i].quality > best_quality) {
            best_quality = segments[i].quality;
            best_idx = i;
        }
    }
    if (count < n_samples && best_idx != 0 && segments[best_idx].midi_note != samples[0].midi_note) {
        samples[count].start_time = segments[best_idx].start_time;
        samples[count].duration = segments[best_idx].duration;
        samples[count].pitch_hz = segments[best_idx].pitch_hz;
        samples[count].midi_note = segments[best_idx].midi_note;
        samples[count].usable_range_semitones = 5.0f;
        snprintf(samples[count].name, 63, "high_%d", samples[count].midi_note);
        count++;
    }
    
    return count;
}
