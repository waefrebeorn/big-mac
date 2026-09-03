/* wbus_param_track.h — parameter (keyframe) animation tracks.
 *
 * One normalized, time-addressed param channel, shared by FX automation,
 * OFX/VST3/CLAP plugin params, and compositor node inputs (R013/R016).
 * Interpolation: hold, linear, bezier (with per-key in/out tangents).
 * "Valid-clamp": times outside the keyed range clamp to the nearest
 * key (no wild extrapolation) unless explicitly set to extrapolate.
 */

#ifndef WUBUS_WBUS_PARAM_TRACK_H
#define WUBUS_WBUS_PARAM_TRACK_H


#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

typedef enum {
    WB_KF_HOLD = 0,
    WB_KF_LINEAR,
    WB_KF_BEZIER,
    WB_KF_HERMITE,
    WB_KF_EASE_IN,
    WB_KF_EASE_OUT,
    WB_KF_EASE_INOUT,
    WB_KF_ELASTIC,
    WB_KF_BOUNCE,
    WB_KF_BACK,
    WB_KF_EXPONENTIAL,
    WB_KF_LOGARITHMIC,
    WB_KF_SCURVE,
    WB_KF_TCB,
    WB_KF_STEP,
    WB_KF_COUNT
} wb_kf_interp;

typedef struct wb_keyframe {
    double t;          /* time in seconds */
    float  value;
    wb_kf_interp interp; /* interpolation TO the NEXT key */
    /* bezier control tangents (normalized -1..1) for WB_KF_BEZIER */
    float in_tangent, out_tangent;
    float in_weight, out_weight;
} wb_keyframe;

typedef struct wb_param_track wb_param_track;

wb_param_track *wb_param_track_create(void);
void            wb_param_track_free(wb_param_track *tr);

/* Add/replace a keyframe at time t. If a key exists at ~t it is replaced. */
void wb_param_track_set(wb_param_track *tr, double t, float value,
                        wb_kf_interp interp);
/* Set bezier tangents for the key at time t (no-op if not present). */
void wb_param_track_set_tangents(wb_param_track *tr, double t,
                                 float in_t, float out_t,
                                 float in_w, float out_w);
/* Remove the key at time t (if any). */
void wb_param_track_remove(wb_param_track *tr, double t);
int  wb_param_track_count(const wb_param_track *tr);
/* G24: keyframe-graph editor accessors */
int  wb_param_track_key_at(const wb_param_track *tr, double t, wb_keyframe *out);
int  wb_param_track_key_index(const wb_param_track *tr, int i, wb_keyframe *out);
void wb_param_track_move_key(wb_param_track *tr, double from_t,
                             double to_t, float value);

/* Evaluate the track at time t.
 * Returns the clamped/extrapolated value. With valid-clamp (default),
 * t < first.key.t returns first.value; t > last.key.t returns last.value. */
float wb_param_track_value_at(const wb_param_track *tr, double t);

/* Set whether values outside the keyed range extrapolate linearly
 * (0 = clamp/hold at endpoints, 1 = linear-extrapolate). Default 0. */
void wb_param_track_set_extrapolate(wb_param_track *tr, int on);


/* R073 hop 82: build a track from uniform time/value samples (linear). */
void wb_param_track_set_many(wb_param_track *tr, const double *ts,
                             const float *vs, int n, wb_kf_interp interp);

#ifdef __cplusplus
}
#endif
#endif /* WUBUS_WBUS_PARAM_TRACK_H */
