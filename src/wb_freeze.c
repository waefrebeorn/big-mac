/* wb_freeze.c — track freeze / bounce-in-place.
 *
 * R077: CPU optimization — render track to audio, disable plugins.
 *
 * Algorithm:
 *   1. Render full track through insert chain to temp buffer
 *   2. Replace live chain with buffer playback
 *   3. On unfreeze: restore original chain
 *   4. Cache invalidation on parameter change
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define FREEZE_MAX_TRACKS 32
#define FREEZE_NAME_LEN 64

typedef struct {
    char     name[FREEZE_NAME_LEN];
    float   *buffer_l;
    float   *buffer_r;
    uint32_t length;         /* Length in samples */
    uint32_t sr;
    int      frozen;
    uint32_t hash;           /* Parameter hash for cache invalidation */
    void    *original_chain; /* Saved insert chain (opaque) */
} frozen_track_t;

typedef struct {
    frozen_track_t tracks[FREEZE_MAX_TRACKS];
    int            num_tracks;
    float         *temp_buffer_l;
    float         *temp_buffer_r;
    uint32_t       temp_size;
} wb_freeze_inst;

void *wb_freeze_create(void) {
    wb_freeze_inst *fr = (wb_freeze_inst *)calloc(1, sizeof(*fr));
    if (!fr) return NULL;
    fr->temp_size = 48000 * 60 * 5;  /* 5 minutes max */
    fr->temp_buffer_l = (float *)calloc(fr->temp_size, sizeof(float));
    fr->temp_buffer_r = (float *)calloc(fr->temp_size, sizeof(float));
    if (!fr->temp_buffer_l || !fr->temp_buffer_r) {
        free(fr->temp_buffer_l);
        free(fr->temp_buffer_r);
        free(fr);
        return NULL;
    }
    return fr;
}

void wb_freeze_destroy(void *inst) {
    wb_freeze_inst *fr = (wb_freeze_inst *)inst;
    if (!fr) return;
    for (int i = 0; i < fr->num_tracks; i++) {
        free(fr->tracks[i].buffer_l);
        free(fr->tracks[i].buffer_r);
    }
    free(fr->temp_buffer_l);
    free(fr->temp_buffer_r);
    free(fr);
}

/* Freeze a track: render it to a buffer.
 * track_name: identifier
 * render_callback: function that renders the track (user-provided)
 * length: number of samples to render
 * sr: sample rate */
int wb_freeze_track(void *inst, const char *track_name,
                     void (*render_callback)(float *, float *, uint32_t, void *),
                     void *render_ctx,
                     uint32_t length, uint32_t sr) {
    wb_freeze_inst *fr = (wb_freeze_inst *)inst;
    if (!fr) return -1;

    /* Find or create track slot */
    int slot = -1;
    for (int i = 0; i < fr->num_tracks; i++) {
        if (strcmp(fr->tracks[i].name, track_name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (fr->num_tracks >= FREEZE_MAX_TRACKS) return -1;
        slot = fr->num_tracks++;
        strncpy(fr->tracks[slot].name, track_name, FREEZE_NAME_LEN - 1);
    }

    frozen_track_t *t = &fr->tracks[slot];

    /* Reallocate buffer if needed */
    if (t->buffer_l) free(t->buffer_l);
    if (t->buffer_r) free(t->buffer_r);
    t->buffer_l = (float *)calloc(length, sizeof(float));
    t->buffer_r = (float *)calloc(length, sizeof(float));
    t->length = length;
    t->sr = sr;

    /* Render through callback */
    if (render_callback) {
        render_callback(t->buffer_l, t->buffer_r, length, render_ctx);
    }

    t->frozen = 1;
    return slot;
}

/* Unfreeze a track. */
int wb_unfreeze_track(void *inst, const char *track_name) {
    wb_freeze_inst *fr = (wb_freeze_inst *)inst;
    if (!fr) return -1;

    for (int i = 0; i < fr->num_tracks; i++) {
        if (strcmp(fr->tracks[i].name, track_name) == 0) {
            fr->tracks[i].frozen = 0;
            return i;
        }
    }
    return -1;
}

/* Check if a track is frozen. */
int wb_is_frozen(void *inst, const char *track_name) {
    wb_freeze_inst *fr = (wb_freeze_inst *)inst;
    if (!fr) return 0;

    for (int i = 0; i < fr->num_tracks; i++) {
        if (strcmp(fr->tracks[i].name, track_name) == 0) {
            return fr->tracks[i].frozen;
        }
    }
    return 0;
}

/* Read frozen audio for playback.
 * Returns 1 if track is frozen and data was read, 0 otherwise. */
int wb_freeze_read(void *inst, const char *track_name,
                    float *out_l, float *out_r,
                    uint32_t n_samples) {
    wb_freeze_inst *fr = (wb_freeze_inst *)inst;
    if (!fr) return 0;

    for (int i = 0; i < fr->num_tracks; i++) {
        if (strcmp(fr->tracks[i].name, track_name) == 0) {
            if (!fr->tracks[i].frozen) return 0;
            uint32_t to_copy = n_samples < fr->tracks[i].length ? n_samples : fr->tracks[i].length;
            memcpy(out_l, fr->tracks[i].buffer_l, to_copy * sizeof(float));
            memcpy(out_r, fr->tracks[i].buffer_r, to_copy * sizeof(float));
            return 1;
        }
    }
    return 0;
}
