/* wbus_compositor.h — pull-based, RoI/RoD node compositor (R013 D1/D3).
 *
 * Design (converged from Natron/Olive/VapourSynth/AVISynth research):
 *   - ONE recursive call: pull(node, t, roi) -> frame
 *   - each node clips the requested RoI to its own RoD (region of definition)
 *   - isIdentity short-circuits no-ops (VirtualDub-style bypass)
 *   - a cache node auto-inserted at graph edges memoizes by content hash
 *     (Hash64(time, roi, node-id)) with BOUNDED LRU eviction
 *   - two-phase: request inputs (phase 0), compute when ready (phase 1)
 *
 * RGBA float frames. Intentionally small + C11; the same contract scales
 * to OFX plugin nodes later (RoI/tile + identity skip already match OFX).
 */

#ifndef WUBUS_WBUS_COMPOSITOR_H
#define WUBUS_WBUS_COMPOSITOR_H

#include <stdint.h>
#include <stddef.h>

/* RGBA float pixel */
typedef struct { float r, g, b, a; } wb_px;

/* A frame: w*h RGBA. roi = region of interest actually filled. */
typedef struct wb_frame {
    int w, h;
    int roi_x, roi_y, roi_w, roi_h;  /* valid sub-rect */
    wb_px *px;                        /* w*h pixels */
} wb_frame;

typedef enum {
    WB_NODE_SOURCE = 0,   /* wraps a producer (e.g. decoded clip) */
    WB_NODE_EFFECT,       /* applies an op to its input(s) */
    WB_NODE_CACHE,        /* auto-inserted memoization wrapper */
    WB_NODE_COMPOSITE     /* blends inputs by alpha (layer stack) */
} wb_node_kind;

typedef struct wb_node wb_node;

/* callbacks the host supplies */
typedef wb_frame* (*wb_node_pull_fn)(wb_node *self, double t,
                                     int rx, int ry, int rw, int rh, int phase);
typedef void (*wb_node_free_fn)(wb_node *self);

#include "wbus/wbus_param_track.h"
#include "wbus/wbus.h"   /* wb_automation_lane bridge (G11 unified bus) */

struct wb_node {
    wb_node_kind kind;
    char id[32];
    int   n_inputs;
    wb_node **inputs;       /* up to N inputs */
    void  *user;            /* node-specific state */
    wb_node_pull_fn pull;
    wb_node_free_fn free;
    /* identity optimization: set during request-phase to skip compute */
    int is_identity;
    /* G11: named keyframed parameters on this node (the shared bus).
     * The compositor pulls param tracks per-frame so FX params animate.
     * A param slot is fed by EITHER a keyframed track OR a session
     * automation lane (both are the same unified bus; track overrides lane). */
    wb_param_track **params;
    wb_automation_lane **param_lanes;
    int   n_params;
    char  param_names[16][32];
};

wb_frame *wb_frame_alloc(int w, int h);
void      wb_frame_free(wb_frame *f);
/* clip roi to (0,0,w,h); returns 0 if roi is empty */
int  wb_roi_clip(int w, int h, int *rx, int *ry, int *rw, int *rh);

/* generic pull entry: handles identity short-circuit + forwarding. */
wb_frame *wb_node_pull(wb_node *n, double t, int rx, int ry, int rw, int rh);

/* ---- G11 param bus: bind a keyframed track to a named node param ------ */
/* Returns the param slot index (>=0), or -1. Name stored for debugging. */
int  wb_node_add_param(wb_node *n, const char *name, wb_param_track *tr);

/* G11 unified bus: bind a session automation LANE directly to a node param.
 * The node reads wb_automation_value_at(lane, t) each pull, so recorded
 * audio automation and video-FX params ride the SAME channel. Precedence:
 * a keyframed track param (if present) overrides the lane. */
int  wb_node_add_param_lane(wb_node *n, const char *name, wb_automation_lane *lane);

/* Get the animated value of a node param at time t (0 if unset). */
float wb_node_param_value(const wb_node *n, const char *name, double t);

/* ---- convenience node factories ------------------------------------- */
/* SOURCE: returns a solid color or a (future) decoded producer.
 * color producer for now (deterministic, testable). */
wb_node *wb_node_source_color(float r, float g, float b, float a, int w, int h);

/* EFFECT: applies a simple op over its single input.
 * op: 0 = identity(bypass), 1 = brightness*gain (gain is keyframable
 *      via param "gain"), 2 = invert-alpha matte */
wb_node *wb_node_effect(int op, float gain);

/* COMPOSITE: blends up to 8 inputs (bottom..top) by alpha (over operator). */
wb_node *wb_node_composite(void);

/* CACHE: wraps a child node; memoizes by hash(time,roi,child-id),
 * bounded LRU (max_frames). */
wb_node *wb_node_cache(wb_node *child, int max_frames);

void wb_node_destroy(wb_node *n);

/* attach an input to a composite (caller keeps ownership of child) */
void wb_composite_add(wb_node *comp, wb_node *child);

#endif /* WUBUS_WBUS_COMPOSITOR_H */
