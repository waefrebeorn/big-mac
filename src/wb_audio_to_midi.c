/* wb_audio_to_midi.c — monophonic audio-to-MIDI conversion.
 *
 * Converts a monophonic audio signal to MIDI note events using YIN pitch
 * detection and energy-based onset detection.
 *
 * Algorithm:
 *   1. Split audio into overlapping frames (2048 samples, 512 hop)
 *   2. Run YIN pitch detection per frame
 *   3. Detect note onsets via energy change
 *   4. When pitch is stable for >min_duration, create a MIDI note
 *   5. Apply silence gate (skip frames below threshold)
 *   6. Output sorted wb_note array
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define A2M_FRAME_SIZE  2048
#define A2M_HOP_SIZE    512
#define A2M_DEFAULT_THRESHOLD  0.02f
#define A2M_DEFAULT_MIN_DUR_MS 50.0f

/* YIN pitch detector (from wb_yin.c) */
extern float wb_yin_pitch(const float *buf, int n, uint32_t sr);

typedef struct wb_audio_to_midi {
    uint32_t sr;
    float    threshold;      /* silence gate threshold (RMS) */
    float    min_dur_ms;     /* minimum note duration in ms */
} wb_audio_to_midi;

wb_audio_to_midi* wb_audio_to_midi_create(uint32_t sr) {
    wb_audio_to_midi *a = (wb_audio_to_midi *)calloc(1, sizeof(wb_audio_to_midi));
    if (!a) return NULL;
    a->sr = sr;
    a->threshold = A2M_DEFAULT_THRESHOLD;
    a->min_dur_ms = A2M_DEFAULT_MIN_DUR_MS;
    return a;
}

void wb_audio_to_midi_destroy(wb_audio_to_midi *a) {
    free(a);
}

void wb_audio_to_midi_set_threshold(wb_audio_to_midi *a, float threshold) {
    if (a) a->threshold = threshold;
}

void wb_audio_to_midi_set_min_duration(wb_audio_to_midi *a, float min_dur_ms) {
    if (a) a->min_dur_ms = min_dur_ms;
}

/* Compute RMS energy of a frame */
static float frame_rms(const float *buf, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += (double)buf[i] * (double)buf[i];
    }
    return (float)sqrt(sum / (double)n);
}

/* Map frequency to MIDI note number */
static uint8_t freq_to_midi(float freq) {
    if (freq <= 0.0f) return 0;
    float midi = 69.0f + 12.0f * log2f(freq / 440.0f);
    int m = (int)roundf(midi);
    if (m < 0) m = 0;
    if (m > 127) m = 127;
    return (uint8_t)m;
}

/* Compare function for sorting notes by start time */
static int note_cmp(const void *a, const void *b) {
    const wb_note *na = (const wb_note *)a;
    const wb_note *nb = (const wb_note *)b;
    if (na->start < nb->start) return -1;
    if (na->start > nb->start) return 1;
    return 0;
}

int wb_audio_to_midi_convert(wb_audio_to_midi *a, const wb_sample *audio,
                              uint32_t frames, wb_note *out_notes,
                              int max_notes, int *out_count) {
    if (!a || !audio || !out_notes || !out_count || frames == 0) {
        if (out_count) *out_count = 0;
        return -1;
    }

    *out_count = 0;
    int note_count = 0;

    uint32_t frame_size = A2M_FRAME_SIZE;
    uint32_t hop_size = A2M_HOP_SIZE;

    /* If signal is shorter than frame_size, pad with zeros */
    if (frames < frame_size) frame_size = frames;

    /* Track current note state */
    int in_note = 0;              /* are we inside a note? */
    double note_start = 0;        /* start sample of current note */
    float note_pitch_freq = 0;    /* accumulated pitch for averaging */
    int pitch_samples = 0;        /* number of frames in current note */
    float prev_energy = 0.0f;     /* previous frame's energy */
    uint32_t min_dur_samples = (uint32_t)(a->min_dur_ms * (float)a->sr / 1000.0f);
    if (min_dur_samples < hop_size) min_dur_samples = hop_size;

    /* Temporary buffer for frame (zero-padded if needed) */
    float frame_buf[A2M_FRAME_SIZE];

    for (uint32_t pos = 0; pos + frame_size <= frames; pos += hop_size) {
        /* Copy frame */
        memcpy(frame_buf, audio + pos, frame_size * sizeof(float));

        /* Compute RMS energy */
        float energy = frame_rms(frame_buf, (int)frame_size);

        /* Silence gate */
        if (energy < a->threshold) {
            /* If we were in a note, close it */
            if (in_note) {
                double note_dur = (double)pos - note_start;
                if (note_dur >= (double)min_dur_samples && note_count < max_notes) {
                    wb_note *n = &out_notes[note_count];
                    n->start = note_start;
                    n->dur = note_dur;
                    float avg_pitch = note_pitch_freq / (float)pitch_samples;
                    n->pitch = freq_to_midi(avg_pitch);
                    /* Velocity from peak energy (rough mapping) */
                    float vel = energy * 4.0f;
                    if (vel > 1.0f) vel = 1.0f;
                    n->vel = (uint8_t)(vel * 127.0f);
                    if (n->vel < 1) n->vel = 1;
                    note_count++;
                }
                in_note = 0;
                note_pitch_freq = 0.0f;
                pitch_samples = 0;
            }
            prev_energy = energy;
            continue;
        }

        /* Detect pitch */
        float pitch = wb_yin_pitch(frame_buf, (int)frame_size, a->sr);

        /* Onset detection: energy increase indicates new note */
        float energy_ratio = (prev_energy > 1e-10f) ? energy / prev_energy : 1.0f;
        int new_onset = (energy_ratio > 2.0f) || (energy > a->threshold * 3.0f && !in_note);

        /* Also detect pitch change as a new note onset (for sweeps) */
        if (in_note && pitch > 0.0f && pitch_samples > 0) {
            float avg_so_far = note_pitch_freq / (float)pitch_samples;
            float pitch_ratio = pitch / avg_so_far;
            /* If pitch changed by more than a semitone (6%), treat as new note */
            if (pitch_ratio > 1.06f || pitch_ratio < 0.94f) {
                new_onset = 1;
            }
        }

        if (pitch > 0.0f) {
            if (!in_note || new_onset) {
                /* Close previous note if exists */
                if (in_note) {
                    double note_dur = (double)pos - note_start;
                    if (note_dur >= (double)min_dur_samples && note_count < max_notes) {
                        wb_note *n = &out_notes[note_count];
                        n->start = note_start;
                        n->dur = note_dur;
                        float avg_pitch = note_pitch_freq / (float)pitch_samples;
                        n->pitch = freq_to_midi(avg_pitch);
                        float vel = energy * 4.0f;
                        if (vel > 1.0f) vel = 1.0f;
                        n->vel = (uint8_t)(vel * 127.0f);
                        if (n->vel < 1) n->vel = 1;
                        note_count++;
                    }
                }
                /* Start new note */
                in_note = 1;
                note_start = (double)pos;
                note_pitch_freq = pitch;
                pitch_samples = 1;
            } else {
                /* Continue current note — accumulate pitch for averaging */
                note_pitch_freq += pitch;
                pitch_samples++;
            }
        }

        prev_energy = energy;
    }

    /* Close any remaining note */
    if (in_note && note_count < max_notes) {
        double note_dur = (double)frames - note_start;
        if (note_dur >= (double)min_dur_samples) {
            wb_note *n = &out_notes[note_count];
            n->start = note_start;
            n->dur = note_dur;
            float avg_pitch = note_pitch_freq / (float)pitch_samples;
            n->pitch = freq_to_midi(avg_pitch);
            n->vel = 64; /* default velocity */
            note_count++;
        }
    }

    /* Sort notes by start time */
    if (note_count > 1) {
        qsort(out_notes, (size_t)note_count, sizeof(wb_note), note_cmp);
    }

    *out_count = note_count;
    return 0;
}