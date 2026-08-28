/* wb_tape_stop.c — tape stop / vinyl brake / slow-down effect.
 *
 * R077: Essential for DJ transitions, meme editing, YTP.
 *
 * Algorithm:
 *   Gradually reduce playback speed (pitch + tempo together).
 *   Models a tape machine or vinyl record stopping.
 *   Uses a variable delay line with decreasing read speed.
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define TAPE_MAX_DELAY (44100 * 3)  /* 3 seconds max */

typedef struct {
    uint32_t sr;
    float   *delay_line;     /* Circular buffer */
    uint32_t delay_size;     /* Buffer size */
    uint32_t write_pos;      /* Write position */
    double   read_pos;       /* Read position (fractional) */
    float    speed;          /* Current playback speed (1.0 = normal) */
    float    target_speed;   /* Target speed (0 = stopped) */
    float    decay_rate;     /* How fast speed decreases */
    int      active;         /* Is effect active */
} wb_tape_stop_inst;

void *wb_tape_stop_create(uint32_t sr) {
    wb_tape_stop_inst *ts = (wb_tape_stop_inst *)calloc(1, sizeof(*ts));
    if (!ts) return NULL;
    ts->sr = sr;
    ts->delay_size = sr * 3;  /* 3 seconds */
    ts->delay_line = (float *)calloc(ts->delay_size, sizeof(float));
    if (!ts->delay_line) { free(ts); return NULL; }
    ts->write_pos = 0;
    ts->read_pos = 0;
    ts->speed = 1.0f;
    ts->target_speed = 0.0f;
    ts->decay_rate = 0.995f;  /* ~0.5 sec to stop */
    ts->active = 0;
    return ts;
}

void wb_tape_stop_destroy(void *inst) {
    wb_tape_stop_inst *ts = (wb_tape_stop_inst *)inst;
    if (ts) { free(ts->delay_line); free(ts); }
}

/* Trigger the tape stop effect */
void wb_tape_stop_trigger(void *inst, float duration_seconds) {
    wb_tape_stop_inst *ts = (wb_tape_stop_inst *)inst;
    if (!ts) return;
    ts->active = 1;
    ts->speed = 1.0f;
    ts->target_speed = 0.0f;
    /* Compute decay rate: speed * decay^N = 0.01 after duration */
    int n_samples = (int)(duration_seconds * ts->sr);
    if (n_samples > 0) {
        ts->decay_rate = powf(0.01f, 1.0f / (float)n_samples);
    }
}

/* Reset to normal playback */
void wb_tape_stop_reset(void *inst) {
    wb_tape_stop_inst *ts = (wb_tape_stop_inst *)inst;
    if (!ts) return;
    ts->active = 0;
    ts->speed = 1.0f;
    ts->read_pos = 0;
}

/* Process a mono block. */
void wb_tape_stop_process(void *inst, float *out, int n) {
    wb_tape_stop_inst *ts = (wb_tape_stop_inst *)inst;
    if (!ts) return;

    for (int i = 0; i < n; i++) {
        /* Write current input to delay line */
        ts->delay_line[ts->write_pos] = out[i];  /* Use output as input (feedback) */
        ts->write_pos = (ts->write_pos + 1) % ts->delay_size;

        if (ts->active) {
            /* Read at current speed */
            uint32_t idx = (uint32_t)ts->read_pos;
            float frac = (float)(ts->read_pos - (double)idx);
            uint32_t idx1 = idx % ts->delay_size;
            uint32_t idx2 = (idx + 1) % ts->delay_size;

            /* Linear interpolation */
            out[i] = ts->delay_line[idx1] + frac * (ts->delay_line[idx2] - ts->delay_line[idx1]);

            /* Advance read position at current speed */
            ts->read_pos += (double)ts->speed;

            /* Decay speed */
            ts->speed *= ts->decay_rate;
            if (ts->speed < 0.01f) {
                ts->speed = 0.0f;
                ts->active = 0;
            }
        } else {
            /* Normal playback: read = write */
            ts->read_pos = (double)ts->write_pos;
        }
    }
}

/* Process stereo (interleaved). */
void wb_tape_stop_process_stereo(void *inst, float *out, int n_frames) {
    wb_tape_stop_inst *ts = (wb_tape_stop_inst *)inst;
    if (!ts) return;

    /* Process L and R separately */
    float *mono = (float *)malloc(n_frames * sizeof(float));
    if (!mono) return;

    /* Mix to mono for tape stop, then apply to both channels */
    for (int i = 0; i < n_frames; i++) {
        mono[i] = (out[i*2] + out[i*2+1]) * 0.5f;
    }

    wb_tape_stop_process(inst, mono, n_frames);

    for (int i = 0; i < n_frames; i++) {
        out[i*2] = mono[i];
        out[i*2+1] = mono[i];
    }

    free(mono);
}
