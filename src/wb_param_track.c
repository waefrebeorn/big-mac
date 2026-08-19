/* wb_param_track.c — keyframe animation track (R016 S2).
 * Pure C11. Keys kept sorted by time. Bezier via de Casteljau on the
 * normalized (t,value) segment using per-key control weights. */

#include "wbus/wbus_param_track.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct wb_param_track {
    wb_keyframe *keys;
    int   count;
    int   cap;
    int   extrapolate;  /* 0 clamp, 1 linear-extrap */
};

wb_param_track *wb_param_track_create(void) {
    wb_param_track *tr = calloc(1, sizeof(*tr));
    return tr;
}

void wb_param_track_free(wb_param_track *tr) {
    if (!tr) return;
    free(tr->keys);
    free(tr);
}

static int key_cmp(const void *a, const void *b) {
    double ta = ((const wb_keyframe*)a)->t;
    double tb = ((const wb_keyframe*)b)->t;
    return (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
}

static int find_index(const wb_param_track *tr, double t) {
    /* returns index i such that keys[i].t <= t < keys[i+1].t, or -1 */
    int lo = 0, hi = tr->count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tr->keys[mid].t <= t) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

void wb_param_track_set(wb_param_track *tr, double t, float value,
                        wb_kf_interp interp) {
    if (!tr) return;
    /* replace existing key near t */
    for (int i = 0; i < tr->count; i++) {
        if (fabs(tr->keys[i].t - t) < 1e-6) {
            tr->keys[i].value = value;
            tr->keys[i].interp = interp;
            return;
        }
    }
    if (tr->count >= tr->cap) {
        int ncap = tr->cap ? tr->cap * 2 : 8;
        wb_keyframe *nk = realloc(tr->keys, ncap * sizeof(wb_keyframe));
        if (!nk) return;
        tr->keys = nk; tr->cap = ncap;
    }
    wb_keyframe k;
    memset(&k, 0, sizeof(k));
    k.t = t; k.value = value; k.interp = interp;
    tr->keys[tr->count++] = k;
    qsort(tr->keys, tr->count, sizeof(wb_keyframe), key_cmp);
}

void wb_param_track_set_tangents(wb_param_track *tr, double t,
                                 float in_t, float out_t,
                                 float in_w, float out_w) {
    if (!tr) return;
    for (int i = 0; i < tr->count; i++) {
        if (fabs(tr->keys[i].t - t) < 1e-6) {
            tr->keys[i].in_tangent = in_t; tr->keys[i].out_tangent = out_t;
            tr->keys[i].in_weight = in_w; tr->keys[i].out_weight = out_w;
            return;
        }
    }
}

void wb_param_track_remove(wb_param_track *tr, double t) {
    if (!tr) return;
    for (int i = 0; i < tr->count; i++) {
        if (fabs(tr->keys[i].t - t) < 1e-6) {
            memmove(&tr->keys[i], &tr->keys[i+1],
                    (tr->count - i - 1) * sizeof(wb_keyframe));
            tr->count--;
            return;
        }
    }
}

int wb_param_track_count(const wb_param_track *tr) {
    return tr ? tr->count : 0;
}

void wb_param_track_set_extrapolate(wb_param_track *tr, int on) {
    if (tr) tr->extrapolate = on ? 1 : 0;
}

/* smoothstep-ish bezier on value given normalized x in [0,1] between
 * two keys; uses out_tangent of a and in_tangent of b as control points. */
static float bezier_seg(float va, float vb, float out_t, float in_t, float x) {
    /* control point values: a + out_t/3 * (range), b + in_t/3 * (range) */
    float range = vb - va;
    float c1 = va + out_t * range / 3.0f;
    float c2 = vb + in_t  * range / 3.0f;
    float omt = 1.0f - x;
    return va*omt*omt*omt + 3.0f*c1*omt*omt*x + 3.0f*c2*omt*x*x + vb*x*x*x;
}

float wb_param_track_value_at(const wb_param_track *tr, double t) {
    if (!tr || tr->count == 0) return 0.0f;
    if (tr->count == 1) return tr->keys[0].value;

    int i = find_index(tr, t);
    if (i < 0) {
        /* t before first key */
        if (tr->extrapolate) {
            const wb_keyframe *a = &tr->keys[0], *b = &tr->keys[1];
            float slope = (b->value - a->value) / (float)(b->t - a->t);
            return a->value + slope * (float)(t - a->t);
        }
        return tr->keys[0].value;
    }
    if (i >= tr->count - 1) {
        /* t after last key */
        if (tr->extrapolate) {
            const wb_keyframe *a = &tr->keys[tr->count-2], *b = &tr->keys[tr->count-1];
            float slope = (b->value - a->value) / (float)(b->t - a->t);
            return b->value + slope * (float)(t - b->t);
        }
        return tr->keys[tr->count-1].value;
    }
    const wb_keyframe *a = &tr->keys[i];
    const wb_keyframe *b = &tr->keys[i+1];
    double dt = b->t - a->t;
    if (dt <= 0) return b->value;
    float x = (float)((t - a->t) / dt);

    switch (a->interp) {
    case WB_KF_HOLD:   return a->value;
    case WB_KF_LINEAR: return a->value + (b->value - a->value) * x;
    case WB_KF_BEZIER: return bezier_seg(a->value, b->value,
                                         a->out_tangent, b->in_tangent, x);
    default:           return a->value;
    }
}
