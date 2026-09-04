/* wb_ytpmv_prod.c — YTPMV Production Pipeline (R104).
 *
 * The actual workflow for making YTPMV:
 * 1. Load source audio (voice clip)
 * 2. Detect phonemes (energy-based segmentation)
 * 3. Detect pitch per phoneme (autocorrelation)
 * 4. Create sampler instrument with phoneme samples
 * 5. Build piano roll melody from pitch-corrected phonemes
 * 6. Map each note to matching video clip
 * 7. Composite final video with beat-synced effects
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
 * PITCH DETECTION (Autocorrelation)
 * ================================================================
 *
 * Detects the fundamental frequency of an audio segment.
 * This is what FL Studio's Newtone does.
 */

float wb_detect_pitch(const float *audio, int n_frames, int sample_rate,
                       float min_freq, float max_freq) {
    if (!audio || n_frames < 2) return 0;
    
    int min_lag = (int)(sample_rate / max_freq);
    int max_lag = (int)(sample_rate / min_freq);
    if (max_lag > n_frames / 2) max_lag = n_frames / 2;
    if (min_lag < 1) min_lag = 1;
    
    float best_corr = 0;
    int best_lag = 0;
    
    for (int lag = min_lag; lag <= max_lag; lag++) {
        float corr = 0;
        float norm = 0;
        int count = n_frames - lag;
        for (int i = 0; i < count; i++) {
            corr += audio[i] * audio[i + lag];
            norm += audio[i] * audio[i] + audio[i + lag] * audio[i + lag];
        }
        if (norm > 0) corr = corr * 2.0f / norm;
        if (corr > best_corr) {
            best_corr = corr;
            best_lag = lag;
        }
    }
    
    if (best_corr < 0.1f) return 0; /* unpitched */
    return (float)sample_rate / best_lag;
}

/* ================================================================
 * PHONEME PITCH CORRECTION
 * ================================================================
 *
 * Detects the pitch of a phoneme and calculates the ratio needed
 * to tune it to the nearest note in a scale.
 */

/* (wb_pitch_correction defined in wbus_compositor.h) */

wb_pitch_correction wb_correct_pitch(float detected_hz, int scale_type) {
    wb_pitch_correction pc = {0};
    pc.original_pitch = detected_hz;
    
    if (detected_hz <= 0) {
        pc.ratio = 1.0f;
        return pc;
    }
    
    /* Map to MIDI note */
    float midi_f = 69 + 12 * log2f(detected_hz / 440.0f);
    int midi = (int)(midi_f + 0.5f);
    
    /* Quantize to scale */
    static const int major[] = {0,2,4,5,7,9,11};
    static const int minor[] = {0,2,3,5,7,8,10};
    static const int chrom[] = {0,1,2,3,4,5,6,7,8,9,10,11};
    const int *scale = (scale_type == 1) ? minor : (scale_type == 0 ? major : chrom);
    int n_scale = (scale_type == 2) ? 12 : 7;
    
    int octave = midi / 12;
    int pc_note = midi % 12;
    int best = pc_note, best_dist = 99;
    for (int i = 0; i < n_scale; i++) {
        int dist = abs(pc_note - scale[i]);
        if (dist > 6) dist = 12 - dist;
        if (dist < best_dist) { best_dist = dist; best = scale[i]; }
    }
    
    pc.midi_note = octave * 12 + best;
    pc.target_pitch = 440.0f * powf(2, (pc.midi_note - 69) / 12.0f);
    pc.ratio = pc.target_pitch / detected_hz;
    pc.cents_off = 1200 * log2f(detected_hz / pc.target_pitch);
    
    return pc;
}

/* ================================================================
 * YTPMV PRODUCER
 * ================================================================
 *
 * The main production state. Ties together:
 * - Source audio/video
 * - Phoneme detection
 * - Pitch correction
 * - Sampler instrument
 * - Piano roll melody
 * - Video clip mapping
 */

/* (ytpmv_producer defined in wbus_compositor.h) */

void ytpmv_prod_init(ytpmv_producer *p, float sample_rate) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->sample_rate = sample_rate;
    p->bpm = 120;
    p->scale_type = 2; /* chromatic default */
    p->base_note = 60;
}

/* Process source audio: detect phonemes + pitches + build melody */
int ytpmv_prod_analyze(ytpmv_producer *p, const float *audio, int n_frames, int n_channels) {
    if (!p || !audio) return 0;
    
    p->source_audio = (float *)audio;
    p->source_frames = n_frames;
    p->source_channels = n_channels;
    
    /* Step 1: Detect phoneme boundaries */
    int segs[YTPMV_MAX_PHONEMES];
    int n_segs = wb_extract_phonemes(audio, n_frames, n_channels, p->sample_rate, segs, YTPMV_MAX_PHONEMES);
    
    /* Step 2: For each segment, detect pitch and correct to scale */
    int prev = 0;
    p->n_phonemes = 0;
    
    for (int i = 0; i <= n_segs && p->n_phonemes < YTPMV_MAX_PHONEMES; i++) {
        int end = (i < n_segs) ? segs[i] : n_frames;
        if (end <= prev) continue;
        
        int seg_len = end - prev;
        if (seg_len < 100) { prev = end; continue; } /* skip very short segments */
        
        /* Mix to mono for pitch detection */
        float *mono = (float *)malloc(seg_len * sizeof(float));
        if (!mono) break;
        for (int j = 0; j < seg_len; j++) {
            float s = 0;
            for (int c = 0; c < n_channels; c++)
                s += audio[(prev + j) * n_channels + c];
            mono[j] = s / n_channels;
        }
        
        /* Detect pitch */
        float pitch = wb_detect_pitch(mono, seg_len, (int)p->sample_rate, 80, 800);
        free(mono);
        
        /* Correct pitch to scale */
        wb_pitch_correction pc = wb_correct_pitch(pitch, p->scale_type);
        
        p->segments[p->n_phonemes] = prev;
        p->pitches[p->n_phonemes] = pitch;
        p->corrections[p->n_phonemes] = pc;
        p->midi_notes[p->n_phonemes] = pc.midi_note;
        p->start_times[p->n_phonemes] = (float)prev / p->sample_rate;
        p->durations[p->n_phonemes] = (float)seg_len / p->sample_rate;
        p->velocities[p->n_phonemes] = 0.8f;
        
        p->n_phonemes++;
        prev = end;
    }
    
    return p->n_phonemes;
}

/* Render the melody to audio output using sampler + piano roll */
int ytpmv_prod_render(ytpmv_producer *p, float *output, int out_frames, int out_channels) {
    if (!p || !output) return 0;
    
    /* For each phoneme, render its audio at the corrected pitch */
    int pos = 0;
    for (int i = 0; i < p->n_phonemes && pos < out_frames; i++) {
        wb_pitch_correction *pc = &p->corrections[i];
        int src_start = p->segments[i];
        int src_end = (i + 1 < p->n_phonemes) ? p->segments[i + 1] : p->source_frames;
        int src_len = src_end - src_start;
        
        /* Resample with pitch correction ratio */
        int out_len = (int)(src_len / pc->ratio);
        if (pos + out_len > out_frames) out_len = out_frames - pos;
        
        for (int j = 0; j < out_len && pos + j < out_frames; j++) {
            float src_pos = j * pc->ratio;
            int src_idx = src_start + (int)src_pos;
            float frac = src_pos - (int)src_pos;
            
            if (src_idx + 1 >= p->source_frames) break;
            
            /* Linear interpolation + velocity */
            for (int c = 0; c < out_channels && c < p->source_channels; c++) {
                float s = p->source_audio[(src_idx) * p->source_channels + c] * (1 - frac)
                        + p->source_audio[(src_idx + 1) * p->source_channels + c] * frac;
                output[(pos + j) * out_channels + c] += s * p->velocities[i];
            }
        }
        pos += out_len;
    }
    
    return pos;
}
