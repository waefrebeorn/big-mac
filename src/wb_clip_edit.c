/* wbus_clip_edit.c — R043 (G1/G2/G8/G9): clip-edit handle side-table.
 *
 * Self-contained: only depends on its header + stdlib. Stores per-clip
 * fade/offset state keyed by (track, clip) so the core wb_clip struct stays
 * layout-stable. Consulted by the engine render path (wb_clip_edit_env) and
 * the UI (wb_clip_edit_get for drag edits). C11 only. */

#include <stdlib.h>
#include <math.h>
#include "wbus/wbus_clip_edit.h"
#include "wbus/wbus_param_track.h"

/* Table layout: a sparse-ish 2D store. We keep one `wb_clip_edit*` per track,
 * each a growable array indexed by clip. Neutral entries are calloc'd (all
 * zeros = no fade, zero offset). */
struct wb_clip_edit_table {
    wb_clip_edit **tracks;   /* array of per-track arrays */
    int           *cap;      /* capacity (clip count) per track */
    int            ntr;      /* number of tracks allocated */
};

wb_clip_edit_table *wb_clip_edit_create(void) {
    wb_clip_edit_table *t = (wb_clip_edit_table*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->ntr = 0;
    return t;
}

void wb_clip_edit_destroy(wb_clip_edit_table *t) {
    if (!t) return;
    for (int i = 0; i < t->ntr; i++) {
        if (t->tracks && t->tracks[i]) free(t->tracks[i]);
    }
    free(t->tracks);
    free(t->cap);
    free(t);
}

static wb_clip_edit *ensure(wb_clip_edit_table *t, int track, int clip) {
    if (!t || track < 0 || clip < 0) return NULL;
    /* grow the track list if needed */
    if (track >= t->ntr) {
        int nntr = track + 4;
        wb_clip_edit **nt = (wb_clip_edit**)realloc(t->tracks, nntr * sizeof(*nt));
        int *nc = (int*)realloc(t->cap, nntr * sizeof(*nc));
        if (!nt || !nc) return NULL;
        for (int i = t->ntr; i < nntr; i++) { nt[i] = NULL; nc[i] = 0; }
        t->tracks = nt; t->cap = nc; t->ntr = nntr;
    }
    /* grow this track's clip array if needed */
    if (clip >= t->cap[track]) {
        int ncap = clip + 4;
        wb_clip_edit *na = (wb_clip_edit*)realloc(t->tracks[track],
                                                 ncap * sizeof(wb_clip_edit));
        if (!na) return NULL;
        for (int i = t->cap[track]; i < ncap; i++)
            na[i].fade_in = na[i].fade_out = na[i].pre_fade_in = 0.0f,
            na[i].start_in_source = 0.0, na[i].loop = 0, na[i].loop_len = 0.0,
            na[i].curve = 0, na[i].color = 0, na[i].retime = 1.0,
            na[i].mc_group = 0, na[i].mc_angle = 0;
        t->tracks[track] = na; t->cap[track] = ncap;
    }
    return &t->tracks[track][clip];
}

wb_clip_edit *wb_clip_edit_get(wb_clip_edit_table *t, int track, int clip) {
    return ensure(t, track, clip);
}

void wb_clip_edit_clear(wb_clip_edit_table *t, int track, int clip) {
    wb_clip_edit *e = ensure(t, track, clip);
    if (e) { e->fade_in = e->fade_out = e->pre_fade_in = 0.0f; e->start_in_source = 0.0; }
}

void wb_clip_edit_move(wb_clip_edit_table *t, int src_track, int src_clip,
                       int dst_track, int dst_clip) {
    if (!t || (src_track == dst_track && src_clip == dst_clip)) return;
    if (src_track < 0 || src_clip < 0 || dst_track < 0 || dst_clip < 0) return;
    wb_clip_edit *src = ensure(t, src_track, src_clip);
    wb_clip_edit *dst = ensure(t, dst_track, dst_clip);
    if (!src || !dst) return;
    *dst = *src;
    src->fade_in = src->fade_out = src->pre_fade_in = 0.0f;
    src->start_in_source = 0.0; src->loop = 0; src->loop_len = 0.0;
    src->curve = 0;
}

float wb_clip_edit_env(const wb_clip_edit *e, double f, double length,
                       double sample_rate) {
    if (!e) return 1.0f;
    float fin = e->fade_in;
    float fout = e->fade_out;
    /* G9 pre-fade: material BEFORE the clip's true start ramps 0->1 over
     * pre_fade_in seconds and reaches full amp exactly at the edit point.
     * Negative f (pre-roll region) is scaled by the ramp; at/after f>=0 the
     * pre-fade is done (full gain) so the true start is untouched. */
    float pre = e->pre_fade_in;
    if (f < 0.0) {
        if (pre <= 0.0f) return 0.0f;   /* no pre-roll authorized -> silence */
        double sec = -f / sample_rate;  /* seconds before the edit point */
        float env = (float)(1.0 - sec / (double)pre);
        if (env < 0.0f) env = 0.0f;
        return env;
    }
    if (fin <= 0.0f && fout <= 0.0f) return 1.0f;   /* neutral: full gain */
    double sec  = f / sample_rate;
    double dur  = length / sample_rate;
    /* G64: curve shape applied to the raw linear fraction t of each ramp.
     *   linear      t            (equal-gain; sum = 1 at crossfade midpoint)
     *   equal-power sqrt(t)      (constant power: sqrt(a)+sqrt(1-a) pairs)
     *   smoothstep  3t^2 - 2t^3  (S-curve, zero slope at both ends) */
    int cv = e->curve;
    if (cv < 0 || cv > 2) cv = 0;
    float env = 1.0f;
    if (fin > 0.0f && sec < fin) {
        double t = sec / fin;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        double g = (cv == 1) ? sqrt(t)
                 : (cv == 2) ? (t * t * (3.0 - 2.0 * t))
                 : t;
        env *= (float)g;
    }
    if (fout > 0.0f && (dur - sec) < fout) {
        double u = (dur - sec) / fout;   /* 1 -> 0 across the fade-out */
        if (u < 0.0) u = 0.0;
        if (u > 1.0) u = 1.0;
        double g = (cv == 1) ? sqrt(u)
                 : (cv == 2) ? (u * u * (3.0 - 2.0 * u))
                 : u;
        env *= (float)g;
    }
    if (env < 0.0f) env = 0.0f;
    if (env > 1.0f) env = 1.0f;
    return env;
}

/* ---- R073 hop 54: keyframable speed ramps ---------------------------------- */
int wb_session_set_retime_ramp(wb_clip_edit_table *et, int track, int clip,
                               wb_param_track *speed) {
    if (!et || speed) { /* et required; speed may be NULL to unbind */ }
    wb_clip_edit *e = wb_clip_edit_get(et, track, clip);
    if (!e) return -1;
    e->ramp = speed;   /* NOT owned — caller keeps the curve alive */
    return 0;
}

double wb_session_retime_source_time(const wb_clip_edit_table *et,
                                     int track, int clip,
                                     double tl_offset) {
    if (!et || tl_offset < 0) return tl_offset;
    const wb_clip_edit *e = wb_clip_edit_get((wb_clip_edit_table*)et,
                                             track, clip);
    if (!e) return tl_offset;
    if (!e->ramp) {
        double r = e->retime > 0.01 ? e->retime : 1.0;
        return tl_offset * r;                    /* constant rate */
    }
    /* numeric integration of speed(t) over [0, tl_offset] in 1/100s steps */
    const double STEP = 0.01;
    double src = 0.0;
    for (double t = 0; t < tl_offset; t += STEP)
        src += wb_param_track_value_at(e->ramp, t + STEP*0.5) * STEP;
    return src;
}
