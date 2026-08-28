/* wb_beat_sync.c — beat-synced editing engine.
 *
 * Music video / meme editing: automatically cut, zoom, flash on beats.
 *
 * Algorithm:
 *   1. Onset detection (spectral flux)
 *   2. Beat tracking (tempo estimation via autocorrelation)
 *   3. Beat-quantized cut generation
 *   4. Beat-triggered effect triggers (zoom, flash, glitch, etc.)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define BS_MAX_BEATS 4096
#define BS_HOP_SIZE 512
#define BS_HISTORY 43  /* ~1 second of energy history at 44.1k */

typedef struct {
    uint32_t sr;
    /* Onset detection */
    float    prev_spectrum[256];
    float    energy_history[BS_HISTORY];
    int      energy_pos;
    /* Beat tracking */
    float    beat_period;    /* in samples */
    float    beat_phase;
    float    bpm;
    /* Output */
    int      beat_count;
    float    beat_times[BS_MAX_BEATS];  /* beat positions in seconds */
    float    beat_strength[BS_MAX_BEATS];
    /* Tempo */
    float    tempo;
} wb_beat_sync_inst;

void *wb_beat_sync_create(uint32_t sr) {
    wb_beat_sync_inst *bs = (wb_beat_sync_inst *)calloc(1, sizeof(*bs));
    if (!bs) return NULL;
    bs->sr = sr;
    bs->beat_period = (float)sr * 0.5f;  /* default 120 BPM */
    bs->bpm = 120.0f;
    return bs;
}

void wb_beat_sync_destroy(void *inst) { free(inst); }

/* Compute spectral flux onset strength for a frame.
 * Returns 0..1 onset strength. */
float wb_beat_sync_onset(const float *samples, int n, wb_beat_sync_inst *bs) {
    if (!bs || n < 64) return 0.0f;

    /* Simple energy-based onset (full-band) */
    float energy = 0;
    for (int i = 0; i < n; i++) {
        energy += samples[i] * samples[i];
    }
    energy /= (float)n;

    /* Compare to running average */
    float avg = 0;
    for (int i = 0; i < BS_HISTORY; i++) avg += bs->energy_history[i];
    avg /= (float)BS_HISTORY;

    /* Spectral flux approximation: positive difference */
    float flux = (energy > avg * 1.3f) ? (energy / (avg + 1e-10f) - 1.0f) : 0.0f;
    if (flux > 1.0f) flux = 1.0f;
    if (flux < 0.0f) flux = 0.0f;

    /* Update history */
    bs->energy_history[bs->energy_pos] = energy;
    bs->energy_pos = (bs->energy_pos + 1) % BS_HISTORY;

    return flux;
}

/* Process audio buffer, detect beats.
 * Returns number of beats detected in this buffer. */
int wb_beat_sync_process(wb_beat_sync_inst *bs, const float *mono,
                          int n_frames, float *beat_times_out) {
    if (!bs || !mono) return 0;

    int hop = BS_HOP_SIZE;
    int n_hops = n_frames / hop;
    int beats_found = 0;

    for (int h = 0; h < n_hops; h++) {
        float onset = wb_beat_sync_onset(mono + h * hop, hop, bs);

        /* Simple beat tracking: if onset is strong and we're past the minimum interval */
        bs->beat_phase += (float)hop;
        float min_interval = bs->beat_period * 0.7f;

        if (onset > 0.3f && bs->beat_phase >= min_interval) {
            /* Beat detected! */
            if (bs->beat_count < BS_MAX_BEATS) {
                float t = (float)(h * hop) / (float)bs->sr;
                bs->beat_times[bs->beat_count] = t;
                bs->beat_strength[bs->beat_count] = onset;
                if (beat_times_out) {
                    beat_times_out[beats_found] = t;
                }
                beats_found++;
                bs->beat_count++;
            }
            bs->beat_phase = 0;

            /* Adapt tempo from recent beat spacing */
            if (bs->beat_count >= 4) {
                float avg_spacing = 0;
                int n_spacing = 0;
                for (int i = bs->beat_count - 3; i < bs->beat_count; i++) {
                    avg_spacing += bs->beat_times[i] - bs->beat_times[i-1];
                    n_spacing++;
                }
                avg_spacing /= (float)n_spacing;
                if (avg_spacing > 0.2f && avg_spacing < 2.0f) {
                    bs->tempo = 60.0f / avg_spacing;
                    bs->beat_period = avg_spacing * (float)bs->sr;
                    bs->bpm = bs->tempo;
                }
            }
        }
    }

    return beats_found;
}

/* Get the current estimated BPM */
float wb_beat_sync_get_bpm(wb_beat_sync_inst *bs) {
    return bs ? bs->bpm : 0.0f;
}

/* Get total beats detected */
int wb_beat_sync_get_beat_count(wb_beat_sync_inst *bs) {
    return bs ? bs->beat_count : 0;
}

/* Get beat times array */
const float* wb_beat_sync_get_beats(wb_beat_sync_inst *bs) {
    return bs ? bs->beat_times : NULL;
}

/* Generate beat-synced edit cuts.
 * Given a source duration and desired cuts-per-beat, output cut points. */
int wb_beat_sync_generate_cuts(wb_beat_sync_inst *bs,
                                float src_duration,
                                int cuts_per_beat,
                                float *cut_times, int max_cuts) {
    if (!bs || !cut_times || bs->beat_count < 2) return 0;

    int n_cuts = 0;
    for (int i = 0; i < bs->beat_count - 1 && n_cuts < max_cuts; i++) {
        float t0 = bs->beat_times[i];
        float t1 = bs->beat_times[i + 1];
        float beat_dur = t1 - t0;

        for (int c = 0; c < cuts_per_beat && n_cuts < max_cuts; c++) {
            float frac = (float)c / (float)cuts_per_beat;
            cut_times[n_cuts++] = t0 + frac * beat_dur;
        }
    }

    return n_cuts;
}

/* Beat-triggered effect: returns an effect intensity 0..1 for a given time.
 * Effect types: 0=zoom, 1=flash, 2=glitch, 3=shake */
float wb_beat_sync_effect_at(wb_beat_sync_inst *bs, float time_sec,
                              int effect_type) {
    if (!bs || bs->beat_count == 0) return 0.0f;

    /* Find nearest beat */
    float nearest_beat_time = 0;
    float min_dist = 1.0f;
    for (int i = 0; i < bs->beat_count; i++) {
        float dist = fabsf(bs->beat_times[i] - time_sec);
        if (dist < min_dist) {
            min_dist = dist;
            nearest_beat_time = bs->beat_times[i];
        }
    }

    /* Time since last beat */
    float since_beat = time_sec - nearest_beat_time;
    if (since_beat < 0) since_beat = -since_beat;

    /* Beat duration */
    float beat_dur = 60.0f / bs->bpm;
    float norm_time = since_beat / beat_dur;  /* 0 at beat, 1 at next beat */

    switch (effect_type) {
    case 0: /* zoom: quick zoom in on beat, ease out */
        return (norm_time < 0.3f) ? (1.0f - norm_time / 0.3f) : 0.0f;
    case 1: /* flash: brief white flash on beat */
        return (norm_time < 0.1f) ? (1.0f - norm_time / 0.1f) : 0.0f;
    case 2: /* glitch: random glitch near beat */
        return (norm_time < 0.15f) ? (1.0f - norm_time / 0.15f) * 0.7f : 0.0f;
    case 3: /* shake: camera shake decaying from beat */
        return (norm_time < 0.4f) ? (1.0f - norm_time / 0.4f) * 0.5f : 0.0f;
    default:
        return 0.0f;
    }
}
