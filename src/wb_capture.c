/* wb_capture.c — G93 capture-quantize + G94 record-session-to-arrangement.
 * Self-contained C11 module (stdlib only). See wbus_capture.h. */
#include "wbus/wbus_capture.h"
#include "wbus/wbus.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WB_CAPLOG_CAP 4096

struct wb_caplog {
    wb_capnote buf[WB_CAPLOG_CAP];
    int        n;        /* valid entries (saturates at CAP) */
    int        head;     /* next write index */
};

wb_caplog *wb_caplog_create(void) {
    return calloc(1, sizeof(wb_caplog));
}

void wb_caplog_destroy(wb_caplog *l) { free(l); }

void wb_caplog_note(wb_caplog *l, double t_samples, int track,
                    int pitch, int vel) {
    if (!l) return;
    l->buf[l->head].t = t_samples;
    l->buf[l->head].track = track;
    l->buf[l->head].pitch = (uint8_t)pitch;
    l->buf[l->head].vel = (uint8_t)vel;
    l->head = (l->head + 1) % WB_CAPLOG_CAP;
    if (l->n < WB_CAPLOG_CAP) l->n++;
}

int wb_caplog_count(const wb_caplog *l) { return l ? l->n : 0; }

const wb_capnote *wb_caplog_at(const wb_caplog *l, int i) {
    if (!l || i < 0 || i >= l->n) return NULL;
    int start = (l->head - l->n + WB_CAPLOG_CAP * 2) % WB_CAPLOG_CAP;
    return &l->buf[(start + i) % WB_CAPLOG_CAP];
}

void wb_caplog_clear(wb_caplog *l) {
    if (!l) return;
    l->n = 0; l->head = 0;
}

/* G93 ------------------------------------------------------------------ */
int wb_capture_quantize(wb_caplog *l, wb_session *s, int track,
                        double t_now, double win_secs, double bpm) {
    if (!l || !s || !s->tracks) return -1;
    if (track < 0 || track >= (int)s->track_count) return -1;
    if (win_secs <= 0.0) win_secs = 4.0;
    double t0 = t_now - win_secs * (double)WB_SAMPLE_RATE;
    if (t0 < 0) t0 = 0;

    /* Collect + quantize candidates first so an empty window leaves no clip. */
    double step = (60.0 / (bpm > 0 ? bpm : 120.0)) / 4.0;   /* 16th, seconds */
    double q[WB_CAPLOG_CAP];
    uint8_t pitch[WB_CAPLOG_CAP], vel[WB_CAPLOG_CAP];
    int nq = 0;
    for (int i = 0; i < wb_caplog_count(l); i++) {
        const wb_capnote *e = wb_caplog_at(l, i);
        if (!e || e->track != track || e->vel == 0) continue;  /* onsets only */
        if (e->t < t0 || e->t > t_now) continue;
        double rel_sec = (e->t - t0) / (double)WB_SAMPLE_RATE;
        double qs = floor(rel_sec / step + 0.5) * step;         /* nearest 16th */
        if (qs >= win_secs) qs = win_secs - step;
        if (qs < 0) qs = 0;
        int dup = 0;   /* retrigs inside one grid slot keep the first */
        for (int k = 0; k < nq; k++)
            if (pitch[k] == (uint8_t)e->pitch &&
                fabs(q[k] - qs * (double)WB_SAMPLE_RATE) < 1.0)
                { dup = 1; break; }
        if (dup) continue;
        q[nq]     = qs * (double)WB_SAMPLE_RATE;
        pitch[nq] = e->pitch;
        vel[nq]   = e->vel;
        nq++;
    }
    if (nq == 0) return -1;

    wb_track *tr = &s->tracks[track];
    if (tr->clip_count >= 1024) return -1;   /* sanity cap (matches session) */
    wb_clip *nc = realloc(tr->clips, (tr->clip_count + 1) * sizeof(wb_clip));
    if (!nc) return -1;
    tr->clips = nc;
    memset(&tr->clips[tr->clip_count], 0, sizeof(wb_clip));
    wb_clip *cl = &tr->clips[tr->clip_count++];
    cl->type   = 0;
    cl->start  = t0;
    cl->length = win_secs * (double)WB_SAMPLE_RATE;
    cl->lane   = tr->active_lane;   /* land on the lane being heard */
    cl->clip_gain = 1.0f;
    cl->notes = malloc((size_t)nq * sizeof(wb_note));
    if (!cl->notes) { tr->clip_count--; return -1; }
    for (int k = 0; k < nq; k++) {
        cl->notes[k].start = q[k];
        cl->notes[k].dur   = (double)WB_SAMPLE_RATE * 0.25;
        cl->notes[k].pitch = pitch[k];
        cl->notes[k].vel   = vel[k];
    }
    cl->note_count = (uint32_t)nq;

    double end = cl->start + cl->length;
    if (end > s->length) s->length = end;
    return nq;
}

/* G94 ------------------------------------------------------------------ */
#define WB_LAUNCHREC_MAX_TRACKS 64

struct wb_launchrec {
    int     armed;
    int     last_clip[WB_LAUNCHREC_MAX_TRACKS];
    wb_launch_span spans[256];
    int     nspans;
};

wb_launchrec *wb_launchrec_create(void) {
    return calloc(1, sizeof(wb_launchrec));
}

void wb_launchrec_destroy(wb_launchrec *r) { free(r); }

static int launched_of(const wb_engine *e, int t) {
    return wb_engine_launched_clip((wb_engine *)e, t);
}

void wb_launchrec_start(wb_launchrec *r, const wb_session *s) {
    if (!r) return;
    r->armed = 1;
    r->nspans = 0;
    memset(r->last_clip, -1, sizeof r->last_clip);
    (void)s;   /* baseline is "nothing launched"; live state arrives via poll */
}

int wb_launchrec_poll(wb_launchrec *r, const wb_session *s,
                      const wb_engine *e, double t_now) {
    if (!r || !r->armed || !s || !e) return 0;
    int changes = 0;
    int nt = (int)s->track_count;
    if (nt > WB_LAUNCHREC_MAX_TRACKS) nt = WB_LAUNCHREC_MAX_TRACKS;
    for (int t = 0; t < nt; t++) {
        int cur = launched_of(e, t);
        int prev = (t < WB_LAUNCHREC_MAX_TRACKS) ? r->last_clip[t] : -1;
        if (cur == prev) continue;
        for (int k = 0; k < r->nspans; k++) {
            wb_launch_span *sp = &r->spans[k];
            if (sp->track == t && sp->t_stop < sp->t_start) sp->t_stop = t_now;
        }
        if (cur >= 0 && r->nspans < 256) {
            r->spans[r->nspans].track = t;
            r->spans[r->nspans].clip_idx = cur;
            r->spans[r->nspans].t_start = t_now;
            r->spans[r->nspans].t_stop = -1.0;   /* open */
            r->nspans++;
        }
        r->last_clip[t] = cur;
        changes++;
    }
    return changes;
}

void wb_launchrec_finish(wb_launchrec *r, double t_now) {
    if (!r) return;
    r->armed = 0;
    for (int k = 0; k < r->nspans; k++)
        if (r->spans[k].t_stop < r->spans[k].t_start)
            r->spans[k].t_stop = t_now;
}

int wb_launchrec_span_count(const wb_launchrec *r) { return r ? r->nspans : 0; }

const wb_launch_span *wb_launchrec_span(const wb_launchrec *r, int i) {
    if (!r || i < 0 || i >= r->nspans) return NULL;
    return &r->spans[i];
}

int wb_launchrec_commit(wb_launchrec *r, wb_session *s) {
    if (!r || !s || !s->tracks) return -1;
    int placed = 0;
    for (int k = 0; k < r->nspans; k++) {
        wb_launch_span *sp = &r->spans[k];
        if (sp->t_stop < sp->t_start) sp->t_stop = sp->t_start;
        double span = sp->t_stop - sp->t_start;
        if (span <= 0) continue;
        if (sp->track < 0 || sp->track >= (int)s->track_count) continue;
        wb_track *tr = &s->tracks[sp->track];
        if (sp->clip_idx < 0 || sp->clip_idx >= (int)tr->clip_count) continue;
        wb_clip *src = &tr->clips[sp->clip_idx];
        if (src->type != 0 || src->note_count == 0) continue;   /* MIDI only */

        if (tr->clip_count >= 1024) break;                      /* sanity cap */
        wb_clip *nc = realloc(tr->clips, (tr->clip_count + 1) * sizeof(wb_clip));
        if (!nc) break;
        tr->clips = nc;
        memset(&tr->clips[tr->clip_count], 0, sizeof(wb_clip));
        wb_clip *dst = &tr->clips[tr->clip_count++];
        dst->type = 0;
        dst->start = sp->t_start;
        dst->length = span;
        dst->lane = 0;              /* recorded performance lands on main lane */
        dst->clip_gain = 1.0f;
        dst->notes = NULL;
        dst->note_count = 0;
        double src_len = src->length > 0 ? src->length : 0;
        uint32_t cap = src->note_count *
                       ((uint32_t)(span / (src_len > 0 ? src_len : 1)) + 2);
        dst->notes = malloc(cap * sizeof(wb_note));
        if (!dst->notes) { tr->clip_count--; continue; }
        uint32_t nn = 0;
        for (double off = 0; off < span; off += src_len) {
            for (uint32_t m = 0; m < src->note_count; m++) {
                double ns = off + src->notes[m].start;
                if (ns >= span) break;
                dst->notes[nn].start = ns;
                dst->notes[nn].dur   = src->notes[m].dur;
                dst->notes[nn].pitch = src->notes[m].pitch;
                dst->notes[nn].vel   = src->notes[m].vel;
                nn++;
                if (nn >= cap) goto filled;
            }
            if (src_len <= 0) break;
        }
filled:
        dst->note_count = nn;
        double end = dst->start + dst->length;
        if (end > s->length) s->length = end;
        placed++;
    }
    return placed;
}
