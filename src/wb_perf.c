/* wb_perf.c — live performance engine: decks, capture, deterministic
 * replay (R065).
 *
 * State model: each deck has `fired` + `fade` (its own crossfade weight).
 * A single global fade position drives A/B split rendering. Events mutate
 * this state; replay re-applies them from a clean slate at any t — the
 * event list is the performance.
 */

#include "wbus/wbus_perf.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WB_PERF_MAX_EVENTS 4096

typedef struct {
    double t;
    int    type;          /* wb_perf_event_type */
    int    deck;
    float  v;             /* fade pos or param value */
    int    which;         /* param selector */
} wb_perf_event;

typedef struct {
    wb_mesh *mesh;        /* owned copy */
    int     fired;
    float   param[4];     /* free params (spin speed, hue shift, ...) */
} wb_perf_deck;

struct wb_perf {
    int w, h;
    wb_perf_deck decks[WB_PERF_MAX_DECKS];
    int ndecks;

    float fade;           /* global A/B position 0..1 */
    int   active_a, active_b;   /* which decks are on each side (-1 none) */

    /* capture + event log (the recording IS the same list used for replay) */
    wb_perf_event events[WB_PERF_MAX_EVENTS];
    int nevents;
    int recording;
    double clock;
};

wb_perf *wb_perf_create(int width, int height) {
    if (width <= 0 || height <= 0) return NULL;
    wb_perf *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->w = width; p->h = height;
    p->active_a = p->active_b = -1;
    return p;
}

void wb_perf_free(wb_perf *p) {
    if (!p) return;
    for (int i = 0; i < p->ndecks; i++) wb_mesh_free(p->decks[i].mesh);
    free(p);
}

static void perf_log(wb_perf *p, int type, int deck, float v, int which) {
    if (!p || p->nevents >= WB_PERF_MAX_EVENTS) return;
    wb_perf_event *e = &p->events[p->nevents++];
    e->t = p->clock; e->type = type; e->deck = deck; e->v = v; e->which = which;
}

int wb_perf_add_deck(wb_perf *p, const wb_mesh *m,
                     uint8_t r, uint8_t g, uint8_t b) {
    if (!p || !m || p->ndecks >= WB_PERF_MAX_DECKS) return -1;
    wb_perf_deck *d = &p->decks[p->ndecks];
    memset(d, 0, sizeof(*d));
    d->mesh = wb_mesh_copy(m);
    if (!d->mesh) return -1;
    (void)r; (void)g; (void)b;
    return p->ndecks++;
}

int wb_perf_deck_count(const wb_perf *p) { return p ? p->ndecks : 0; }

/* ---- live control -------------------------------------------------------- */

static int deck_visible(wb_perf *p, int deck) {
    if (deck < 0 || deck >= p->ndecks) return 0;
    /* a deck is on screen if it is the active A or B side and fade favors it */
    if (p->active_a == deck && p->fade < 0.999f) return 1;
    if (p->active_b == deck && p->fade > 0.001f) return 1;
    if (p->active_a == deck && p->active_b != deck && p->fade <= 0.001f) return 1;
    if (p->ndecks == 1 && p->active_a == deck) return 1;
    return 0;
}

int wb_perf_fire(wb_perf *p, int deck) {
    if (!p || deck < 0 || deck >= p->ndecks) return -1;
    if (p->fade < 0.5f) p->active_a = deck; else p->active_b = deck;
    perf_log(p, WB_PERF_FIRE, deck, p->fade, 0);
    return p->nevents - 1;
}
int wb_perf_unfire(wb_perf *p, int deck) {
    if (!p || deck < 0 || deck >= p->ndecks) return -1;
    if (p->active_a == deck) p->active_a = -1;
    if (p->active_b == deck) p->active_b = -1;
    perf_log(p, WB_PERF_UNFIRE, deck, 0, 0);
    return p->nevents - 1;
}
int wb_perf_fade(wb_perf *p, float pos) {
    if (!p) return -1;
    if (pos < 0) pos = 0; if (pos > 1) pos = 1;
    p->fade = pos;
    perf_log(p, WB_PERF_FADE, -1, pos, 0);
    return p->nevents - 1;
}
int wb_perf_param(wb_perf *p, int deck, int which, float v) {
    if (!p || deck < 0 || deck >= p->ndecks || which < 0 || which > 3)
        return -1;
    p->decks[deck].param[which] = v;
    perf_log(p, WB_PERF_PARAM, deck, v, which);
    return p->nevents - 1;
}

/* ---- capture / replay ----------------------------------------------------- */

void wb_perf_record_arm(wb_perf *p)   { if (p) p->recording = 1; }
void wb_perf_record_stop(wb_perf *p)  { if (p) p->recording = 0; }
int  wb_perf_recording(const wb_perf *p) { return p ? p->recording : 0; }
int  wb_perf_event_count(const wb_perf *p) { return p ? p->nevents : 0; }
void wb_perf_set_clock(wb_perf *p, double t) { if (p) p->clock = t; }

void wb_perf_seek(wb_perf *p, double t) {
    if (!p) return;
    /* clean slate */
    p->fade = 0.0f;
    p->active_a = p->active_b = -1;
    for (int i = 0; i < p->ndecks; i++)
        memset(p->decks[i].param, 0, sizeof p->decks[i].param);
    /* apply events in order up to t */
    for (int i = 0; i < p->nevents; i++) {
        const wb_perf_event *e = &p->events[i];
        if (e->t > t) break;
        switch (e->type) {
        case WB_PERF_FIRE: {
            int d = e->deck;
            if (d >= 0 && d < p->ndecks) {
                if (p->fade < 0.5f) p->active_a = d; else p->active_b = d;
            }
            break;
        }
        case WB_PERF_UNFIRE: {
            int d = e->deck;
            if (p->active_a == d) p->active_a = -1;
            if (p->active_b == d) p->active_b = -1;
            break;
        }
        case WB_PERF_FADE:
            p->fade = e->v;
            break;
        case WB_PERF_PARAM:
            if (e->deck >= 0 && e->deck < p->ndecks)
                p->decks[e->deck].param[e->which] = e->v;
            break;
        default: break;
        }
    }
}

/* ---- render ----------------------------------------------------------------- */

void wb_perf_render_frame(wb_perf *p, uint8_t *out_rgba) {
    if (!p || !out_rgba) return;
    memset(out_rgba, 0, (size_t)p->w * p->h * 4);

    /* collect visible decks in fire order */
    int vis[WB_PERF_MAX_DECKS];
    int nvis = 0;
    if (p->active_a >= 0 && deck_visible(p, p->active_a)) vis[nvis++] = p->active_a;
    if (p->active_b >= 0 && p->active_b != p->active_a &&
        deck_visible(p, p->active_b)) vis[nvis++] = p->active_b;
    if (nvis == 0) return;

    /* render each visible deck full-frame; later decks overwrite earlier
     * (crossfade approximation via alpha-stamped overwrite). With one deck
     * visible this is simply full-screen. */
    wb_rast_ctx *r = wb_rast_create(p->w, p->h);
    if (!r) return;
    wb_rast_set_sun(r, 0.45f, 0.75f, 0.5f, 0.9f);

    static uint8_t deck_img[640*360*4];
    size_t need = (size_t)p->w * p->h * 4;
    if (need > sizeof deck_img) { wb_rast_destroy(r); return; }

    double spin = p->decks[vis[0]].param[0];

    for (int k = 0; k < nvis; k++) {
        int di = vis[k];
        wb_perf_deck *d = &p->decks[di];
        /* animate rotation by param[0] (spin) + clock-driven yaw */
        wb_rast_set_camera(r, 0.45f, (float)(spin + p->clock), 0,
                           7.0f / (1.0f + d->param[1]), 0);

        int nv = wb_mesh_vert_count(d->mesh);
        int nt = wb_mesh_tri_count(d->mesh);
        const wb_rast_vertex *vs = wb_mesh_vert_src(d->mesh);
        const wb_rast_tri *ts = wb_mesh_tri_src(d->mesh);
        wb_rast_set_scene(r, vs, nv, ts, nt);

        memset(deck_img, 0, need);
        wb_rast_render(r, deck_img);

        /* composite over output where alpha set */
        for (size_t i = 0; i < (size_t)p->w*p->h; i++) {
            if (deck_img[i*4+3] == 255) {
                out_rgba[i*4+0] = deck_img[i*4+0];
                out_rgba[i*4+1] = deck_img[i*4+1];
                out_rgba[i*4+2] = deck_img[i*4+2];
                out_rgba[i*4+3] = 255;
            }
        }
    }
    wb_rast_destroy(r);
}
