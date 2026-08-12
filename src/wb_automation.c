/* wb_automation.c — automation envelopes (breakpoint curves over time).
 * A lane owns an ordered list of (time,value) breakpoints and interpolates
 * a value at any song position (linear / hold / smooth). The engine applies
 * the interpolated value to a named parameter once per render block.
 *
 * Pure C11, no dependencies.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "wbus.h"

/* ---- automation recording (capture live fader/param moves) ------------ */
/* A recorder accumulates (time,value) samples while the user drags a
 * fader/param during playback. It applies a dead-band so tiny/quantized
 * jitter doesn't create thousands of points; on commit the points are
 * copied into the target lane (replacing its prior contents in range). */
#define WB_AUTO_REC_MAX 8192

struct wb_automation_recorder {
    wb_automation_lane *lane;   /* target lane (owned by caller) */
    double *times;
    double *vals;
    uint32_t count;
    uint32_t cap;
    double  deadband;           /* min |dv| to record a new point */
    double  last_value;         /* last value written */
    int     armed;              /* recording in progress */
};

wb_automation_recorder *wb_automation_recorder_create(wb_automation_lane *lane,
                                                       double deadband) {
    wb_automation_recorder *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->lane = lane;
    r->cap = 256;
    r->deadband = deadband > 0 ? deadband : 0.005;
    r->times = malloc(r->cap * sizeof(double));
    r->vals = malloc(r->cap * sizeof(double));
    if (!r->times || !r->vals) {
        free(r->times); free(r->vals); free(r);
        return NULL;
    }
    return r;
}

void wb_automation_recorder_destroy(wb_automation_recorder *r) {
    if (!r) return;
    free(r->times); free(r->vals); free(r);
}

void wb_automation_recorder_arm(wb_automation_recorder *r, double init_value) {
    if (!r) return;
    r->count = 0;
    r->last_value = init_value;
    r->armed = 1;
}

void wb_automation_recorder_disarm(wb_automation_recorder *r) {
    if (r) r->armed = 0;
}

int wb_automation_recorder_armed(const wb_automation_recorder *r) {
    return r ? r->armed : 0;
}

/* Number of points captured but not yet committed. */
int wb_automation_recorder_count(const wb_automation_recorder *r) {
    return r ? (int)r->count : 0;
}

/* Feed a parameter value observed at song position `pos`. If it differs from
 * the last recorded value by more than the dead-band, a point is appended. */
void wb_automation_recorder_capture(wb_automation_recorder *r, double pos,
                                     double value) {
    if (!r || !r->armed) return;
    /* dead-band: ignore changes below threshold (jitter/quantization) */
    if (r->count > 0 && fabs(value - r->last_value) < r->deadband) return;
    /* ensure capacity */
    if (r->count == r->cap) {
        uint32_t ncap = r->cap * 2;
        if (ncap > WB_AUTO_REC_MAX) return;
        double *nt = realloc(r->times, ncap * sizeof(double));
        double *nv = realloc(r->vals, ncap * sizeof(double));
        if (!nt || !nv) return;
        r->times = nt; r->vals = nv; r->cap = ncap;
    }
    r->times[r->count] = pos;
    r->vals[r->count] = value;
    r->count++;
    r->last_value = value;
}

/* Commit the captured points into the target lane. Points already in the
 * lane at times >= first captured time are removed first (overwrite region),
 * then captured points are added (keeps sorted order). */
int wb_automation_recorder_commit(wb_automation_recorder *r) {
    if (!r || !r->lane || r->count == 0) return 0;
    wb_automation_lane *l = r->lane;
    double t0 = r->times[0];
    /* remove existing points at/after t0 (overwrite the recording region) */
    uint32_t keep = 0;
    for (uint32_t i = 0; i < l->point_count; i++)
        if (l->points[i].time < t0 - 1e-9) l->points[keep++] = l->points[i];
    l->point_count = keep;
    /* add captured points (insertion keeps them sorted) */
    for (uint32_t i = 0; i < r->count; i++)
        wb_automation_add_point(l, r->times[i], r->vals[i], 0 /* linear */);
    r->count = 0;
    return 1;
}


/* ---- lane lifecycle ---------------------------------------------------- */
wb_automation_lane *wb_automation_lane_create(const char *param) {
    wb_automation_lane *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    if (param) snprintf(l->param, sizeof(l->param), "%s", param);
    return l;
}

void wb_automation_lane_destroy(wb_automation_lane *l) {
    if (!l) return;
    free(l->points);
    free(l);
}

/* Add (or replace) a breakpoint at time; keeps points sorted by time. */
int wb_automation_add_point(wb_automation_lane *l, double time, double value, int curve) {
    if (!l) return -1;
    /* find insertion index (keep sorted, replace if same time) */
    uint32_t i = 0;
    while (i < l->point_count && l->points[i].time < time) i++;
    if (i < l->point_count && fabs(l->points[i].time - time) < 1e-9) {
        l->points[i].value = value;
        l->points[i].curve = curve;
        return 0;
    }
    wb_automation_point *np = realloc(l->points, (l->point_count + 1) * sizeof(wb_automation_point));
    if (!np) return -1;
    l->points = np;
    memmove(&l->points[i+1], &l->points[i], (l->point_count - i) * sizeof(wb_automation_point));
    l->points[i].time = time;
    l->points[i].value = value;
    l->points[i].curve = curve;
    l->point_count++;
    return 0;
}

int wb_automation_clear(wb_automation_lane *l) {
    if (!l) return -1;
    l->point_count = 0;
    return 0;
}

/* Interpolate the lane's value at song position `pos` (samples).
 * Returns the interpolated value, or `fallback` if no points bound it. */
double wb_automation_value_at(const wb_automation_lane *l, double pos, double fallback) {
    if (!l || l->point_count == 0) return fallback;
    /* before first point */
    if (pos <= l->points[0].time) return l->points[0].value;
    /* after last point */
    if (pos >= l->points[l->point_count-1].time)
        return l->points[l->point_count-1].value;
    /* find surrounding segment */
    uint32_t i = 0;
    while (i < l->point_count-1 && l->points[i+1].time < pos) i++;
    const wb_automation_point *a = &l->points[i];
    const wb_automation_point *b = &l->points[i+1];
    double span = b->time - a->time;
    if (span < 1e-12) return b->value;
    double t = (pos - a->time) / span;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    switch (a->curve) {
        case 1:  return a->value;                     /* hold */
        case 2: { /* smooth: cosine interp */
            double ct = (1.0 - cos(t * M_PI)) * 0.5;
            return a->value * (1.0 - ct) + b->value * ct;
        }
        default: return a->value * (1.0 - t) + b->value * t; /* linear */
    }
}
