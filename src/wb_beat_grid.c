#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wbus/wbus_compositor.h"

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

#define MAX_BEATS 2048
#define MAX_SUBDIV 4

/* (wb_beat_grid defined in wbus_compositor.h) */

void wb_beat_grid_init(wb_beat_grid *bg, float bpm, float sample_rate, float duration) {
    if (!bg) return;
    memset(bg, 0, sizeof(*bg));
    bg->bpm = bpm > 0 ? bpm : 120.0f;
    bg->sample_rate = sample_rate > 0 ? sample_rate : 44100.0f;
    bg->duration_seconds = duration;
    bg->quantize_strength = 0.8f;
    bg->quantize_division = 4; /* 1/4 notes by default */
    
    /* Compute beat positions */
    float sec_per_beat = 60.0f / bg->bpm;
    int samples_per_beat = (int)(sec_per_beat * bg->sample_rate);
    
    bg->n_beats = 0;
    int pos = 0;
    while (pos < (int)(duration * bg->sample_rate) && bg->n_beats < MAX_BEATS) {
        bg->beat_samples[bg->n_beats++] = pos;
        pos += samples_per_beat;
    }
    
    /* Compute subdivision positions */
    bg->n_subdivs = 0;
    int subdiv_per_beat = bg->quantize_division;
    int samples_per_subdiv = samples_per_beat / subdiv_per_beat;
    
    if (samples_per_subdiv < 1) samples_per_subdiv = 1;
    
    for (int b = 0; b < bg->n_beats && bg->n_subdivs < MAX_BEATS * MAX_SUBDIV - subdiv_per_beat; b++) {
        for (int s = 0; s < subdiv_per_beat; s++) {
            bg->subdiv_samples[bg->n_subdivs++] = bg->beat_samples[b] + s * samples_per_subdiv;
        }
    }
}

/* Quantize a timestamp (in samples) to the nearest grid position */
int wb_beat_grid_quantize(wb_beat_grid *bg, int sample_pos) {
    if (!bg || bg->n_subdivs == 0) return sample_pos;
    
    /* Find nearest subdivision */
    int best = sample_pos;
    int best_dist = bg->sample_rate; /* large initial distance */
    
    for (int i = 0; i < bg->n_subdivs; i++) {
        int dist = abs(bg->subdiv_samples[i] - sample_pos);
        if (dist < best_dist) {
            best_dist = dist;
            best = bg->subdiv_samples[i];
        }
    }
    
    /* Apply quantize strength: lerp between original and quantized */
    int result = (int)(sample_pos + (best - sample_pos) * bg->quantize_strength);
    return result;
}

/* Quantize a timestamp in seconds */
float wb_beat_grid_quantize_sec(wb_beat_grid *bg, float time_sec) {
    if (!bg) return time_sec;
    int sample = (int)(time_sec * bg->sample_rate);
    int quantized = wb_beat_grid_quantize(bg, sample);
    return (float)quantized / bg->sample_rate;
}

/* Snap phoneme start times to the beat grid */
void wb_beat_grid_quantize_phonemes(wb_beat_grid *bg,
                                     float *start_times, float *durations,
                                     int n_phonemes) {
    if (!bg || !start_times || !durations) return;
    
    for (int i = 0; i < n_phonemes; i++) {
        int start_sample = (int)(start_times[i] * bg->sample_rate);
        int dur_samples = (int)(durations[i] * bg->sample_rate);
        
        /* Quantize start to grid */
        int q_start = wb_beat_grid_quantize(bg, start_sample);
        
        /* Quantize duration to nearest subdivision multiple */
        float sec_per_subdiv = (60.0f / bg->bpm) / bg->quantize_division;
        int subdivs = (int)(durations[i] / sec_per_subdiv + 0.5f);
        if (subdivs < 1) subdivs = 1;
        float q_dur = subdivs * sec_per_subdiv;
        
        start_times[i] = (float)q_start / bg->sample_rate;
        durations[i] = q_dur;
    }
}

/* Check if a given time falls on a beat (within tolerance) */
int wb_beat_grid_is_on_beat(wb_beat_grid *bg, float time_sec, float tolerance_sec) {
    if (!bg) return 0;
    int sample = (int)(time_sec * bg->sample_rate);
    int tol_samples = (int)(tolerance_sec * bg->sample_rate);
    
    for (int i = 0; i < bg->n_beats; i++) {
        if (abs(bg->beat_samples[i] - sample) <= tol_samples)
            return 1;
    }
    return 0;
}

/* Get the beat number (0-indexed) for a given time, or -1 if between beats */
int wb_beat_grid_beat_at(wb_beat_grid *bg, float time_sec) {
    if (!bg) return -1;
    int sample = (int)(time_sec * bg->sample_rate);
    
    for (int i = 0; i < bg->n_beats; i++) {
        if (bg->beat_samples[i] == sample)
            return i;
    }
    return -1;
}

/* Generate a rhythmic pattern: which subdivisions to place notes on */
int wb_beat_grid_generate_pattern(wb_beat_grid *bg, int pattern_type,
                                    int *hit_flags, int n_steps) {
    if (!bg || !hit_flags || n_steps <= 0) return 0;
    
    /* Clear */
    memset(hit_flags, 0, n_steps * sizeof(int));
    
    int hits = 0;
    
    switch (pattern_type) {
        case 0: /* Four on the floor */
            for (int i = 0; i < n_steps; i += 4) {
                hit_flags[i] = 1;
                hits++;
            }
            break;
            
        case 1: /* Offbeat 8ths */
            for (int i = 0; i < n_steps; i += 2) {
                hit_flags[i] = 1;
                hits++;
            }
            break;
            
        case 2: /* Syncopated */
            for (int i = 0; i < n_steps; i++) {
                if (i % 3 == 0 || i % 5 == 0) {
                    hit_flags[i] = 1;
                    hits++;
                }
            }
            break;
            
        case 3: /* Euclidean-ish (spread N hits across steps) */
        {
            int n_hits = n_steps / 3;
            if (n_hits < 1) n_hits = 1;
            for (int i = 0; i < n_hits; i++) {
                int idx = (int)((float)i / n_hits * n_steps);
                if (idx < n_steps) {
                    hit_flags[idx] = 1;
                    hits++;
                }
            }
            break;
        }
        
        case 4: /* Every beat */
        default:
            for (int i = 0; i < n_steps; i++) {
                hit_flags[i] = 1;
                hits++;
            }
            break;
    }
    
    return hits;
}
