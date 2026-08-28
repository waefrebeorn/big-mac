/* wb_comping.c — take lanes and comping system.
 *
 * R077: Non-destructive recording with take lanes.
 *
 * Data model:
 *   Track has multiple take lanes (one per recording pass)
 *   Comp lane selects segments from takes
 *   Render walks comp segments, reading from chosen takes
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_TAKES 16
#define MAX_SEGMENTS 64

typedef struct {
    float   *audio;          /* Take audio data */
    uint32_t length;         /* Length in samples */
    int      active;         /* Is this take recorded */
} take_t;

typedef struct {
    int      take_index;     /* Which take this segment reads from */
    uint32_t start_sample;   /* Start in take */
    uint32_t length;         /* Length in samples */
} comp_segment_t;

typedef struct {
    take_t   takes[MAX_TAKES];
    int      num_takes;

    comp_segment_t segments[MAX_SEGMENTS];
    int      num_segments;
    int      active_segment;
} wb_comping_inst;

void *wb_comping_create(void) {
    wb_comping_inst *cp = (wb_comping_inst *)calloc(1, sizeof(*cp));
    return cp;
}

void wb_comping_destroy(void *inst) {
    wb_comping_inst *cp = (wb_comping_inst *)inst;
    if (!cp) return;
    for (int i = 0; i < MAX_TAKES; i++) {
        free(cp->takes[i].audio);
    }
    free(cp);
}

/* Add a new take lane. Returns take index or -1. */
int wb_comping_add_take(void *inst, const float *audio, uint32_t length) {
    wb_comping_inst *cp = (wb_comping_inst *)inst;
    if (!cp || cp->num_takes >= MAX_TAKES) return -1;

    int idx = cp->num_takes++;
    cp->takes[idx].audio = (float *)malloc(length * sizeof(float));
    memcpy(cp->takes[idx].audio, audio, length * sizeof(float));
    cp->takes[idx].length = length;
    cp->takes[idx].active = 1;
    return idx;
}

/* Add a comp segment. */
int wb_comping_add_segment(void *inst, int take_idx, uint32_t start, uint32_t length) {
    wb_comping_inst *cp = (wb_comping_inst *)inst;
    if (!cp || cp->num_segments >= MAX_SEGMENTS) return -1;
    if (take_idx < 0 || take_idx >= cp->num_takes) return -1;

    int idx = cp->num_segments++;
    cp->segments[idx].take_index = take_idx;
    cp->segments[idx].start_sample = start;
    cp->segments[idx].length = length;
    return idx;
}

/* Render comp to output buffer. */
void wb_comping_render(void *inst, float *output, uint32_t output_len) {
    wb_comping_inst *cp = (wb_comping_inst *)inst;
    if (!cp || !output) return;

    memset(output, 0, output_len * sizeof(float));

    uint32_t pos = 0;
    for (int s = 0; s < cp->num_segments && pos < output_len; s++) {
        comp_segment_t *seg = &cp->segments[s];
        if (seg->take_index < 0 || seg->take_index >= cp->num_takes) continue;

        take_t *take = &cp->takes[seg->take_index];
        if (!take->active || !take->audio) continue;

        uint32_t copy_len = seg->length;
        if (pos + copy_len > output_len) copy_len = output_len - pos;
        if (seg->start_sample + copy_len > take->length)
            copy_len = take->length - seg->start_sample;

        memcpy(output + pos, take->audio + seg->start_sample, copy_len * sizeof(float));
        pos += copy_len;
    }
}
