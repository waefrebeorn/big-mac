/* wbus/wbus_transitions_pro.h — professional video transition pack.
 *
 * Object-oriented API for 20+ GPU-style transitions between RGBA frames.
 * Each transition is initialized with a type, processes from/to frames at
 * a normalized progress (0.0 = fully from-frame, 1.0 = fully to-frame),
 * and supports per-instance params + duration for UI scrubbing.
 */

#ifndef WBUS_TRANSITIONS_PRO_H
#define WBUS_TRANSITIONS_PRO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Transition type IDs (20+) ---- */
enum {
    WB_TRANSITION_DISSOLVE = 0,
    WB_TRANSITION_WIPE,
    WB_TRANSITION_SLIDE,
    WB_TRANSITION_ZOOM,
    WB_TRANSITION_SPIN,
    WB_TRANSITION_GLITCH,
    WB_TRANSITION_BLUR,
    WB_TRANSITION_PIXELATE,
    WB_TRANSITION_MOSAIC,
    WB_TRANSITION_FLASH,
    WB_TRANSITION_BURN,
    WB_TRANSITION_RIPPLE,
    WB_TRANSITION_KALEIDOSCOPE,
    WB_TRANSITION_WARP,
    WB_TRANSITION_CROSS_ZOOM,
    WB_TRANSITION_DOORWAY,
    WB_TRANSITION_FRACTAL,
    WB_TRANSITION_SHATTER,
    WB_TRANSITION_VORTEX,
    WB_TRANSITION_CLOCK_WIPE,
    WB_TRANSITION_WHIP_PAN,
    WB_TRANSITION_MORPH,
    WB_TRANSITION_FLIP,
    WB_TRANSITION_CUBE,
    WB_TRANSITION_PUSH,
    WB_TRANSITION_BARN_DOORS,
    WB_TRANSITION_TRANSFORM,
    WB_TRANSITION_COUNT
};

/* ---- Param IDs (transition-specific, set via wb_transition_set_param) ---- */
enum {
    WB_TRANS_PARAM_DIRECTION = 0,   /* 0=right,1=left,2=up,3=down (0..360 deg) */
    WB_TRANS_PARAM_INTENSITY,       /* 0..1: glitch/blur/burn strength */
    WB_TRANS_PARAM_COUNT,           /* 1..N: slices, pieces, divisions */
    WB_TRANS_PARAM_SCALE,           /* 0..2: zoom factor */
    WB_TRANS_PARAM_ANGLE,           /* 0..360: rotation degrees */
    WB_TRANS_PARAM_FEATHER,         /* 0..1: edge softness */
    WB_TRANS_PARAM_CENTER_X,        /* 0..1: normalized center X */
    WB_TRANS_PARAM_CENTER_Y,        /* 0..1: normalized center Y */
    WB_TRANS_PARAM_SEGMENTS,        /* 2..N: radial segments */
    WB_TRANS_PARAM_PARAM_10,        /* reserved */
    WB_TRANS_PARAM_PARAM_11,        /* reserved */
    WB_TRANS_PARAM_COUNT_MAX
};

/* Opaque transition context */
typedef struct wb_transition wb_transition;

/* ---- Public API ---- */

/* Initialize a transition context for the given type and frame dimensions.
 * Returns 0 on success, -1 on invalid args. */
int wb_transition_init(wb_transition *t, int type, int src_w, int src_h);

/* Process one frame: blend from[] into to[] at progress [0..1] into out[].
 * out, from, to are RGBA buffers of width*height*4 bytes.
 * Returns 0 on success, -1 on error. */
int wb_transition_process(wb_transition *t,
                          const uint8_t *from,
                          const uint8_t *to,
                          uint8_t *out,
                          float progress);

/* Set the transition duration in seconds (for UI/scrubbing reference). */
void wb_transition_set_duration(wb_transition *t, float seconds);

/* Set a named parameter on the transition.
 * param: one of WB_TRANS_PARAM_* above.
 * value: parameter value (semantics per-param above). */
void wb_transition_set_param(wb_transition *t, int param, float value);

/* Get the duration in seconds. */
float wb_transition_get_duration(const wb_transition *t);

/* Get a named parameter value. */
float wb_transition_get_param(const wb_transition *t, int param);

/* Get the transition type. */
int wb_transition_get_type(const wb_transition *t);

/* Get a human-readable name for a transition type ID. */
const char *wb_transition_type_name(int type);

/* Release any internal resources (currently a no-op since the struct is
 * stack-allocatable, but provided for future extensibility). */
void wb_transition_destroy(wb_transition *t);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_TRANSITIONS_PRO_H */
