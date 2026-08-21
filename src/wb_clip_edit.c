/* wbus_clip_edit.c — R043 (G1/G2/G8/G9): clip-edit handle side-table.
 *
 * Self-contained: only depends on its header + stdlib. Stores per-clip
 * fade/offset state keyed by (track, clip) so the core wb_clip struct stays
 * layout-stable. Consulted by the engine render path (wb_clip_edit_env) and
 * the UI (wb_clip_edit_get for drag edits). C11 only. */

#include <stdlib.h>
#include "wbus/wbus_clip_edit.h"

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
            na[i].start_in_source = 0.0, na[i].loop = 0, na[i].loop_len = 0.0;
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

float wb_clip_edit_env(const wb_clip_edit *e, double f, double length,
                       double sample_rate) {
    if (!e) return 1.0f;
    float fin = e->fade_in;
    float fout = e->fade_out;
    if (fin <= 0.0f && fout <= 0.0f) return 1.0f;   /* neutral: full gain */
    double sec  = f / sample_rate;
    double dur  = length / sample_rate;
    float env = 1.0f;
    if (fin  > 0.0f && sec < fin)               env *= (float)(sec / fin);
    if (fout > 0.0f && (dur - sec) < fout)      env *= (float)((dur - sec) / fout);
    if (env < 0.0f) env = 0.0f;
    if (env > 1.0f) env = 1.0f;
    return env;
}
