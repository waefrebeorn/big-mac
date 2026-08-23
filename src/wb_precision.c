/* wb_precision.c — Wave 2 lane B: G15 trim nudge, G16 razor, G66 drop modes.
 * Pure C11 session math; no UI, no media I/O (gate-testable headless). */
#include "wbus_precision.h"
#include <stdlib.h>
#include <string.h>

#define WB_PREC_MIN_LEN 0.04      /* one frame at 25 fps */
#define WB_PREC_EPS     1e-4

static wb_clip *prec_clip(wb_session *s, int track, int clip) {
    if (!s || track < 0 || track >= (int)s->track_count) return NULL;
    wb_track *tr = &s->tracks[track];
    if (clip < 0 || (uint32_t)clip >= tr->clip_count) return NULL;
    wb_clip *cl = &tr->clips[clip];
    if (cl->type != 2 || !cl->video) return NULL;
    return cl;
}

static double prec_len(const wb_clip *cl) {
    return cl->length > 0 ? cl->length : cl->video->duration;
}

int wb_precision_nearest_edge(wb_session *s, int track, int clip,
                              double t, int *edge) {
    wb_clip *cl = prec_clip(s, track, clip);
    if (!cl || !edge) return -1;
    double cs = cl->start, ce = cl->start + prec_len(cl);
    *edge = (fabs(t - cs) <= fabs(ce - t)) ? 0 : 1;
    return 0;
}

int wb_session_nudge_edit_point(wb_session *s, int track, int clip,
                                int edge, double delta) {
    if (!s || (edge != 0 && edge != 1) || delta == 0.0) return -1;
    wb_track *tr = &s->tracks[track];   /* prec_clip validated bounds below */
    wb_clip *cl = prec_clip(s, track, clip);
    if (!cl) return -1;

    if (edge == 0) {                                   /* ---- in-point ---- */
        double len = prec_len(cl);
        if (delta > len - WB_PREC_MIN_LEN) delta = len - WB_PREC_MIN_LEN;
        /* never push the source window before 0 */
        if (cl->video->start_in_source >= 0.0 &&
            cl->video->start_in_source + delta < 0.0)
            delta = -cl->video->start_in_source;
        if (delta == 0.0) return 0;
        /* rolling off the head past zero is clamped by the abutment below */
        if (clip > 0) {
            wb_clip *prev = &tr->clips[clip - 1];
            if (prev->type == 2 && prev->video &&
                fabs(prev->start + prec_len(prev) - cl->start) < WB_PREC_EPS)
                prev->length = prec_len(prev) + delta;   /* roll neighbor */
        }
        cl->start += delta;
        cl->length = len - delta;
        if (cl->video->start_in_source >= 0.0)
            cl->video->start_in_source += delta;
        if (cl->video->duration > 0.0) cl->video->duration -= delta;
        cl->video->timeline_pos = cl->start;
    } else {                                           /* ---- out-point --- */
        double len = prec_len(cl);
        double room = 1e9;
        if ((uint32_t)(clip + 1) < tr->clip_count) {
            wb_clip *next = &tr->clips[clip + 1];
            if (next->type == 2 && next->video &&
                fabs(cl->start + len - next->start) < WB_PREC_EPS)
                room = prec_len(next) - WB_PREC_MIN_LEN;
        }
        if (delta > room) delta = room;
        cl->length = len + delta;
        if (cl->video->duration > 0.0) cl->video->duration += delta;
        if (delta != 0.0 && (uint32_t)(clip + 1) < tr->clip_count) {
            wb_clip *next = &tr->clips[clip + 1];
            if (next->type == 2 && next->video &&
                fabs(next->start - (cl->start + len)) < WB_PREC_EPS) {
                next->start += delta;
                next->length = prec_len(next) - delta;
                if (next->video->start_in_source >= 0.0)
                    next->video->start_in_source += delta;
                if (next->video->duration > 0.0)
                    next->video->duration -= delta;
                next->video->timeline_pos = next->start;
            }
        }
    }
    (void)tr;
    return 0;
}

int wb_session_razor_split_all_at_time(wb_session *s, double t,
                                       int track, int all_tracks) {
    if (!s || t < 0.0) return -1;
    int t0 = all_tracks ? 0 : track, t1 = all_tracks ? (int)s->track_count
                                                     : track + 1;
    if (t0 < 0 || t1 > (int)s->track_count) return -1;
    int splits = 0;
    for (int ti = t0; ti < t1; ti++) {
        wb_track *tr = &s->tracks[ti];
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            wb_clip *cl = &tr->clips[c];
            if (cl->type != 2 || !cl->video) continue;
            double cs = cl->start, ce = cs + prec_len(cl);
            if (t > cs + WB_PREC_EPS && t < ce - WB_PREC_EPS) {
                if (wb_session_split_video_clip(s, ti, (int)c, t) >= 0) {
                    splits++;
                    c++;   /* skip the freshly inserted right half */
                }
            }
        }
    }
    return splits;
}

int wb_session_drop_place(wb_session *s, int track, double pos, double len,
                          wb_drop_mode mode) {
    if (!s || track < 0 || track >= (int)s->track_count || len <= 0.0)
        return -1;
    if (mode == WB_DROP_OVERWRITE) return 0;
    wb_track *tr = &s->tracks[track];
    int shifted = 0;
    for (uint32_t c = 0; c < tr->clip_count; c++) {
        wb_clip *cl = &tr->clips[c];
        if (cl->start >= pos - WB_PREC_EPS) { cl->start += len; shifted++; }
    }
    return shifted;
}
