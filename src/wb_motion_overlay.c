/* wb_motion_overlay.c — motion tracking overlay node (R085).
 *
 * Wraps wb_motion_track as a compositor effect node. On pull:
 *   1. Pulls the input frame
 *   2. Runs motion detection/tracking on the RGBA frame
 *   3. Makes tracked point positions available via params
 *   4. Returns the frame (unmodified — tracking is metadata)
 *
 * Other nodes (text, transform) can read the tracked positions
 * to attach graphics to moving objects.
 *
 * Pure C11.
 */

#include "wbus/wbus_compositor.h"
#include "wbus/wbus_vfx.h"
#include <stdlib.h>
#include <string.h>

/* Forward declarations from wb_motion_track.c (no public header) */
typedef struct {
    float x, y;
    float prev_x, prev_y;
    int active;
    int id;
} tracked_point_t;

void *wb_motion_track_create(int width, int height);
void wb_motion_track_destroy(void *inst);
int wb_motion_track_detect(void *inst, const uint8_t *rgba, int num_points);
int wb_motion_track_frame(void *inst, const uint8_t *rgba);
const tracked_point_t* wb_motion_track_get_points(void *inst, int *out_num);

typedef struct {
    void *tracker;
    int w, h;
    int initialized;
    int num_points;
    float pos_x[64];  /* Normalized positions (0..1) */
    float pos_y[64];
} mo_node_t;

static wb_frame *mo_pull(wb_node *self, double t,
                          int rx, int ry, int rw, int rh, int phase) {
    (void)t;
    mo_node_t *mo = self->user;
    if (self->n_inputs < 1) return NULL;
    if (phase == 0) {
        wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh);
        return NULL;
    }
    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;

    int w = in->w, h = in->h;

    /* Re-create tracker if dimensions changed */
    if (mo->w != w || mo->h != h) {
        if (mo->tracker) wb_motion_track_destroy(mo->tracker);
        mo->tracker = wb_motion_track_create(w, h);
        mo->w = w;
        mo->h = h;
        mo->initialized = 0;
    }
    if (!mo->tracker) return in;

    /* Convert float RGBA to uint8 for tracking */
    uint8_t *rgba = malloc(w * h * 4);
    if (!rgba) return in;
    for (int i = 0; i < w * h; i++) {
        rgba[i*4+0] = (uint8_t)(in->px[i].r * 255.0f + 0.5f);
        rgba[i*4+1] = (uint8_t)(in->px[i].g * 255.0f + 0.5f);
        rgba[i*4+2] = (uint8_t)(in->px[i].b * 255.0f + 0.5f);
        rgba[i*4+3] = 255;
    }

    /* Run motion tracking */
    if (!mo->initialized) {
        wb_motion_track_detect(mo->tracker, rgba, 16);
        mo->initialized = 1;
    } else {
        wb_motion_track_frame(mo->tracker, rgba);
    }

    /* Get tracked points */
    int num_points = 0;
    const tracked_point_t *points = wb_motion_track_get_points(mo->tracker, &num_points);
    mo->num_points = num_points > 64 ? 64 : num_points;
    for (int i = 0; i < mo->num_points; i++) {
        mo->pos_x[i] = (float)points[i].x / (float)w;
        mo->pos_y[i] = (float)points[i].y / (float)h;
    }

    free(rgba);
    return in;  /* Frame is unmodified — tracking data is in params */
}

static void mo_free(wb_node *n) {
    mo_node_t *mo = n->user;
    if (mo->tracker) wb_motion_track_destroy(mo->tracker);
    free(mo);
}

wb_node *wb_node_effect_motion_track(void) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "motion_track");
    if (!n) return NULL;
    mo_node_t *mo = calloc(1, sizeof(*mo));
    if (!mo) { wb_node_destroy(n); return NULL; }
    mo->tracker = NULL;
    mo->w = 0;
    mo->h = 0;
    mo->initialized = 0;
    mo->num_points = 0;
    n->user = mo;
    n->pull = mo_pull;
    n->free = mo_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    return n;
}

/* Get tracked point position (for use by other nodes/UI). */
int wb_node_effect_motion_get_points(const wb_node *n, float *xs, float *ys,
                                      int max_points) {
    if (!n || n->kind != WB_NODE_EFFECT) return 0;
    mo_node_t *mo = (mo_node_t *)n->user;
    if (!mo) return 0;
    int npts = mo->num_points < max_points ? mo->num_points : max_points;
    for (int i = 0; i < npts; i++) {
        if (xs) xs[i] = mo->pos_x[i];
        if (ys) ys[i] = mo->pos_y[i];
    }
    return npts;
}
