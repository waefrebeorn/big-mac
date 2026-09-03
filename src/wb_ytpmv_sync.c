/* wb_ytpmv_sync.c — YTPMV sync engine: audio-reactive video + lip sync (R095).
 *
 * The core YTPMV production pipeline:
 * 1. Analyze source audio → detect phonemes, pitch, beats
 * 2. Map phonemes → MIDI notes (pitch-to-note)
 * 3. Quantize to scale/key
 * 4. Generate beat-synced video effects
 * 5. Lip-sync: match mouth shapes to vowel phonemes
 *
 * Also implements After Effects-style audio-to-keyframes:
 * - Audio amplitude → parameter modulation
 * - Beat detection → keyframe triggers
 * - Wiggle expression (freq, amp) driven by audio
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* Include compositor header for phoneme types from wb_ytpmv.c */
#include "wbus/wbus_compositor.h"

/* Types (wb_beat_map, wb_audio_keys, wb_viseme, wb_lip_frame, wb_ytpmv_plan)
 * are defined in wbus/wbus_compositor.h — included above. */

/* ================================================================
 * BEAT DETECTION
 * ================================================================ */

wb_beat_map *wb_beat_map_create(int capacity) {
    wb_beat_map *m = (wb_beat_map *)calloc(1, sizeof(wb_beat_map));
    if (!m) return NULL;
    m->beat_times = (float *)calloc(capacity, sizeof(float));
    if (!m->beat_times) { free(m); return NULL; }
    m->capacity = capacity;
    return m;
}

void wb_beat_map_free(wb_beat_map *m) {
    if (!m) return;
    free(m->beat_times);
    free(m);
}

/* Detect beats from audio envelope */
/* Uses simple energy flux: beat when energy jumps significantly */
int wb_ytpmv_detect_beats(const float *audio, int n_frames, int n_channels,
                     float sample_rate, float threshold, wb_beat_map *out) {
    if (!audio || !out || n_frames <= 0) return 0;

    int window = (int)(sample_rate * 0.01f); /* 10ms analysis window */
    if (window < 1) window = 1;
    int n_windows = n_frames / window;

    /* Compute energy per window */
    float *energy = (float *)calloc(n_windows, sizeof(float));
    if (!energy) return 0;

    float total_energy = 0;
    for (int w = 0; w < n_windows; w++) {
        float e = 0;
        for (int i = w * window; i < (w + 1) * window && i < n_frames; i++) {
            float s = 0;
            for (int c = 0; c < n_channels; c++)
                s += audio[i * n_channels + c] * audio[i * n_channels + c];
            e += s / n_channels;
        }
        energy[w] = e / window;
        total_energy += energy[w];
    }
    out->avg_energy = total_energy / n_windows;

    /* Detect beats: energy flux above threshold * local average */
    float local_avg = 0;
    int local_window = 43; /* ~0.5s at 10ms windows */
    for (int w = local_window; w < n_windows && out->n_beats < out->capacity; w++) {
        /* Update local average */
        local_avg = 0;
        for (int i = w - local_window; i < w; i++)
            local_avg += energy[i];
        local_avg /= local_window;

        /* Check for beat: current energy > threshold * local average */
        if (energy[w] > local_avg * (1.0f + threshold) &&
            energy[w] > energy[w-1] && energy[w] >= energy[w+1]) {
            out->beat_times[out->n_beats++] = (float)w * window / sample_rate;
        }
    }

    /* Estimate BPM from beat intervals */
    if (out->n_beats >= 2) {
        float total_interval = 0;
        int n_intervals = 0;
        for (int i = 1; i < out->n_beats; i++) {
            float interval = out->beat_times[i] - out->beat_times[i-1];
            if (interval > 0.2f && interval < 2.0f) { /* 30-300 BPM range */
                total_interval += interval;
                n_intervals++;
            }
        }
        if (n_intervals > 0)
            out->bpm = 60.0f / (total_interval / n_intervals);
    }

    free(energy);
    return out->n_beats;
}

/* ================================================================
 * AUDIO AMPLITUDE → KEYFRAMES (After Effects style)
 * ================================================================ */

/* wb_audio_keys: defined in wbus_compositor.h */

wb_audio_keys *wb_audio_keys_create(int capacity) {
    wb_audio_keys *k = (wb_audio_keys *)calloc(1, sizeof(wb_audio_keys));
    if (!k) return NULL;
    k->times = (float *)calloc(capacity, sizeof(float));
    k->values = (float *)calloc(capacity, sizeof(float));
    if (!k->times || !k->values) { free(k->times); free(k->values); free(k); return NULL; }
    k->capacity = capacity;
    return k;
}

void wb_audio_keys_free(wb_audio_keys *k) {
    if (!k) return;
    free(k->times);
    free(k->values);
    free(k);
}

/* Generate audio amplitude keyframes (like AE "Convert Audio to Keyframes") */
int wb_audio_to_keyframes(const float *audio, int n_frames, int n_channels,
                           float sample_rate, float interval, wb_audio_keys *out) {
    if (!audio || !out || n_frames <= 0) return 0;

    int frames_per_key = (int)(sample_rate * interval);
    if (frames_per_key < 1) frames_per_key = 1;
    int n_keys = n_frames / frames_per_key;

    for (int k = 0; k < n_keys && k < out->capacity; k++) {
        int start = k * frames_per_key;
        int end = start + frames_per_key;
        if (end > n_frames) end = n_frames;

        /* RMS energy */
        float rms = 0;
        for (int i = start; i < end; i++) {
            float s = 0;
            for (int c = 0; c < n_channels; c++)
                s += audio[i * n_channels + c] * audio[i * n_channels + c];
            rms += s / n_channels;
        }
        rms = sqrtf(rms / (end - start));

        out->times[k] = (float)start / sample_rate;
        out->values[k] = rms;
        out->n_keyframes++;
    }

    /* Normalize to 0..1 */
    float max_val = 0;
    for (int i = 0; i < out->n_keyframes; i++)
        if (out->values[i] > max_val) max_val = out->values[i];
    if (max_val > 0.001f)
        for (int i = 0; i < out->n_keyframes; i++)
            out->values[i] /= max_val;

    return out->n_keyframes;
}

/* ================================================================
 * WIGGLE EXPRESSION (After Effects style)
 * ================================================================ */

/* AE wiggle(freq, amp) simulation */
/* Returns a time-series of wiggle values */
void wb_wiggle(float *out, int n_frames, float sample_rate,
               float frequency, float amplitude, int seed) {
    if (!out || n_frames <= 0) return;

    /* Simplified wiggle: sum of sine waves at harmonic frequencies */
    srand(seed);
    float phase1 = (float)rand() / RAND_MAX * 2.0f * M_PI;
    float phase2 = (float)rand() / RAND_MAX * 2.0f * M_PI;
    float phase3 = (float)rand() / RAND_MAX * 2.0f * M_PI;

    for (int i = 0; i < n_frames; i++) {
        float t = (float)i / sample_rate;
        /* Multi-frequency wiggle for organic feel */
        out[i] = amplitude * (
            0.5f * sinf(2.0f * M_PI * frequency * t + phase1) +
            0.3f * sinf(2.0f * M_PI * frequency * 1.7f * t + phase2) +
            0.2f * sinf(2.0f * M_PI * frequency * 0.3f * t + phase3)
        );
    }
}

/* Audio-driven wiggle: amplitude modulates wiggle strength */
void wb_audio_wiggle(float *out, int n_frames, float sample_rate,
                     float frequency, float base_amplitude,
                     const float *audio_keys, int n_keys, float key_interval) {
    if (!out || n_frames <= 0) return;

    float *wiggle_base = (float *)calloc(n_frames, sizeof(float));
    if (!wiggle_base) return;

    wb_wiggle(wiggle_base, n_frames, sample_rate, frequency, 1.0f, 42);

    for (int i = 0; i < n_frames; i++) {
        float t = (float)i / sample_rate;
        int key_idx = (int)(t / key_interval);
        if (key_idx >= n_keys) key_idx = n_keys - 1;
        float amp = base_amplitude * (0.3f + 0.7f * audio_keys[key_idx]);
        out[i] = wiggle_base[i] * amp;
    }

    free(wiggle_base);
}

/* ================================================================
 * LIP SYNC ENGINE
 * ================================================================ */

/* wb_viseme: defined in wbus_compositor.h */

/* Map phoneme type to viseme */
wb_viseme phoneme_to_viseme(wb_phoneme_type phon) {
    switch (phon) {
        case PHON_VOWEL_A: return VISEME_AH;
        case PHON_VOWEL_E: return VISEME_EE;
        case PHON_VOWEL_I: return VISEME_EE;
        case PHON_VOWEL_O: return VISEME_OH;
        case PHON_VOWEL_U: return VISEME_OO;
        case PHON_CONSONANT_M:
        case PHON_CONSONANT_B:
        case PHON_CONSONANT_P: return VISEME_MBP;
        case PHON_CONSONANT_F:
        case PHON_CONSONANT_V: return VISEME_FV;
        case PHON_CONSONANT_L: return VISEME_L;
        case PHON_CONSONANT_T: return VISEME_TH;
        case PHON_CONSONANT_W: return VISEME_W;
        case PHON_SILENCE: return VISEME_REST;
        default: return VISEME_AH; /* default to open mouth */
    }
}

/* Get viseme name for debugging */
const char *viseme_name(wb_viseme v) {
    static const char *names[] = {
        "REST", "AH", "EE", "OH", "OO", "FV", "MBP", "L", "TH", "W"
    };
    if (v < 0 || v >= VISEME_COUNT) return "???";
    return names[v];
}

/* wb_lip_frame: defined in wbus_compositor.h */

/* Generate lip sync frames from phoneme database */
int wb_generate_lip_sync(const wb_phoneme_db *db, wb_lip_frame *frames, int max_frames) {
    if (!db || !frames || max_frames <= 0) return 0;

    int count = 0;
    for (int i = 0; i < db->count && count < max_frames; i++) {
        wb_phoneme *p = &db->phonemes[i];
        if (p->type == PHON_SILENCE || p->type == PHON_UNKNOWN) continue;

        frames[count].start_time = p->start_time;
        frames[count].end_time = p->end_time;
        frames[count].viseme = phoneme_to_viseme(p->type);
        frames[count].blend = 0.3f; /* 30% blend between visemes */
        count++;
    }
    return count;
}

/* ================================================================
 * BEAT-SYNCED VIDEO EFFECTS
 * ================================================================ */

/* Generate beat-synced zoom pulses */
void wb_beat_sync_zoom(float *zoom_curve, int n_frames, float sample_rate,
                        const wb_beat_map *beats, float pulse_strength) {
    if (!zoom_curve || !beats || n_frames <= 0) return;

    /* Start at 1.0 (no zoom) */
    for (int i = 0; i < n_frames; i++)
        zoom_curve[i] = 1.0f;

    /* Add zoom pulse on each beat */
    for (int b = 0; b < beats->n_beats; b++) {
        int center = (int)(beats->beat_times[b] * sample_rate);
        int decay_frames = (int)(sample_rate * 0.15f); /* 150ms decay */

        for (int i = -decay_frames/2; i < decay_frames/2; i++) {
            int idx = center + i;
            if (idx < 0 || idx >= n_frames) continue;
            float t = (float)i / (decay_frames / 2);
            float pulse = (1.0f - fabsf(t)) * pulse_strength;
            pulse *= expf(-fabsf(t) * 3.0f); /* exponential decay */
            zoom_curve[idx] += pulse;
        }
    }
}

/* Generate beat-synced color flash */
void wb_beat_sync_flash(uint8_t *rgba, int w, int h, int frame_num,
                         float sample_rate, const wb_beat_map *beats,
                         float flash_intensity) {
    if (!rgba || !beats || w <= 0 || h <= 0) return;
    float t = (float)frame_num / sample_rate;

    /* Find nearest beat */
    float min_dist = 999;
    for (int b = 0; b < beats->n_beats; b++) {
        float dist = fabsf(beats->beat_times[b] - t);
        if (dist < min_dist) min_dist = dist;
    }

    /* Flash if close to a beat */
    float flash_window = 0.08f; /* 80ms flash */
    if (min_dist < flash_window) {
        float intensity = (1.0f - min_dist / flash_window) * flash_intensity;
        for (int i = 0; i < w * h; i++) {
            for (int c = 0; c < 3; c++) {
                float val = rgba[i*4+c] / 255.0f;
                val += (1.0f - val) * intensity;
                if (val > 1) val = 1;
                rgba[i*4+c] = (uint8_t)(val * 255);
            }
        }
    }
}

/* ================================================================
 * YTPMV AUTO-PILOT
 * ================================================================ */

/* wb_ytpmv_plan: defined in wbus_compositor.h */

wb_ytpmv_plan *wb_ytpmv_plan_create(void) {
    wb_ytpmv_plan *plan = (wb_ytpmv_plan *)calloc(1, sizeof(wb_ytpmv_plan));
    if (!plan) return NULL;
    plan->phonemes = wb_phoneme_db_create(256);
    plan->beats = wb_beat_map_create(512);
    plan->amp_keys = wb_audio_keys_create(4096);
    plan->lip_frames = (wb_lip_frame *)calloc(256, sizeof(wb_lip_frame));
    plan->scale = SCALE_CHROMATIC;
    plan->root_note = 60;
    return plan;
}

void wb_ytpmv_plan_free(wb_ytpmv_plan *plan) {
    if (!plan) return;
    wb_phoneme_db_free(plan->phonemes);
    wb_beat_map_free(plan->beats);
    wb_audio_keys_free(plan->amp_keys);
    free(plan->lip_frames);
    free(plan);
}

/* Analyze audio and build complete YTPMV plan */
int wb_ytpmv_analyze(wb_ytpmv_plan *plan, const float *audio, int n_frames,
                      int n_channels, float sample_rate) {
    if (!plan || !audio || n_frames <= 0) return -1;

    /* 1. Detect beats */
    wb_ytpmv_detect_beats(audio, n_frames, n_channels, sample_rate, 0.3f, plan->beats);
    plan->bpm = plan->beats->bpm;

    /* 2. Generate amplitude keyframes */
    wb_audio_to_keyframes(audio, n_frames, n_channels, sample_rate,
                           0.04f, plan->amp_keys); /* 40ms interval */

    /* 3. Estimate pitch per window and add phonemes */
    int pitch_window = (int)(sample_rate * 0.05f); /* 50ms windows */
    int n_windows = n_frames / pitch_window;
    for (int w = 0; w < n_windows && plan->phonemes->count < plan->phonemes->capacity; w++) {
        int start = w * pitch_window;
        int end = start + pitch_window;

        /* Simple pitch detection: zero-crossing rate */
        int crossings = 0;
        for (int i = start + 1; i < end && i < n_frames; i++) {
            if ((audio[i*n_channels] >= 0 && audio[(i-1)*n_channels] < 0) ||
                (audio[i*n_channels] < 0 && audio[(i-1)*n_channels] >= 0))
                crossings++;
        }
        float pitch = (float)crossings * sample_rate / (2.0f * pitch_window);
        if (pitch < 50) pitch = 0; /* silence */

        /* Energy */
        float energy = 0;
        for (int i = start; i < end && i < n_frames; i++)
            energy += fabsf(audio[i*n_channels]);
        energy /= pitch_window;

        /* Classify */
        wb_phoneme_type type = PHON_UNKNOWN;
        if (energy < 0.01f) {
            type = PHON_SILENCE;
        } else if (pitch > 0) {
            /* Simple vowel classification by frequency ranges */
            if (pitch > 200 && pitch < 400) type = PHON_VOWEL_A;
            else if (pitch >= 400 && pitch < 600) type = PHON_VOWEL_E;
            else if (pitch >= 600 && pitch < 900) type = PHON_VOWEL_I;
            else if (pitch >= 100 && pitch < 200) type = PHON_VOWEL_O;
            else if (pitch >= 50 && pitch < 100) type = PHON_VOWEL_U;
            else type = PHON_CONSONANT_S;
        }

        wb_phoneme_add(plan->phonemes, (float)start / sample_rate,
                        (float)end / sample_rate, pitch, energy, type);
    }

    /* 4. Generate lip sync frames */
    plan->n_lip_frames = wb_generate_lip_sync(plan->phonemes, plan->lip_frames, 256);

    return 0;
}
