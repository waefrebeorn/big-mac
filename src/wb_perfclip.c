/* wb_perfclip.c — performance recordings as nested timeline clips (R068). */

#include "wbus/wbus_perfclip.h"
#include "wbus/wbus_mesh.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Forward to the shadow-bin's atomic-commit helper (tmp + rename). */
int wb_shadowbin_atomic_commit(const char *tmp, const char *dst);

struct wb_perfclip {
    double start;
    double duration;
    int    deck_count;
    float  color[16][3];
    uint32_t event_count;
    wb_perf_event_view *events;   /* snapshot of the perf event log */
};

wb_perfclip *wb_perfclip_snapshot(wb_session *s, wb_perf *perf,
                                  double start, double duration) {
    (void)s;
    if (!perf) return NULL;
    wb_perfclip *pc = calloc(1, sizeof(*pc));
    if (!pc) return NULL;
    pc->start = start;
    pc->duration = duration;
    pc->deck_count = wb_perf_deck_count(perf);

    pc->event_count = (uint32_t)wb_perf_event_count(perf);
    if (pc->event_count > 0) {
        pc->events = malloc(pc->event_count * sizeof(wb_perf_event_view));
        if (!pc->events) { wb_perfclip_free(pc); return NULL; }
        const wb_perf_event_view *src =
            (const wb_perf_event_view *)wb_perf_event_dump(perf);
        for (uint32_t i = 0; i < pc->event_count; i++) {
            pc->events[i].t    = src[i].t;
            pc->events[i].type = src[i].type;
            pc->events[i].deck = src[i].deck;
            pc->events[i].v    = src[i].v;
            pc->events[i].which = src[i].which;
        }
    }
    return pc;
}

void wb_perfclip_free(wb_perfclip *pc) {
    if (!pc) return;
    free(pc->events);
    free(pc);
}

int wb_perfclip_event_count(const wb_perfclip *pc) {
    return pc ? (int)pc->event_count : 0;
}

int wb_perfclip_render(wb_perfclip *pc, double t,
                       uint8_t *rgba, int w, int h) {
    if (!pc || !rgba || w <= 0 || h <= 0) return -1;
    /* R070: real rasterization — replay the event snapshot into a fresh
     * private wb_perf (deterministic: same events → same pixels) and
     * render its state at t. The live session perf is untouched. */
    wb_perf *tmp = wb_perf_create(w, h);
    if (!tmp) return -1;
    /* seed decks: one per deck in the snapshot (tints carried; meshes
     * default to a box since deck geometry lives with the host perf) */
    for (int d = 0; d < pc->deck_count; d++) {
        uint8_t r = pc->color[d][0], g = pc->color[d][1], b = pc->color[d][2];
        if (!r && !g && !b) { r = 120; g = 120; b = 140; }
        wb_mesh *m = wb_mesh_box(1, 1, 0.2f, r, g, b);
        if (m) { wb_perf_add_deck(tmp, m, r, g, b); wb_mesh_free(m); }
    }
    /* replay the event list */
    wb_perf_reset_for_replay(tmp);
    for (uint32_t i = 0; i < pc->event_count; i++) {
        const wb_perf_event_view *e = &pc->events[i];
        if (e->t > t) break;
        switch (e->type) {
        case WB_PERF_FIRE: {
            /* apply directly to the replay state at the recorded time */
            wb_perf_set_clock(tmp, e->t);
            wb_perf_fire(tmp, e->deck);
            break;
        }
        case WB_PERF_UNFIRE:
            wb_perf_set_clock(tmp, e->t);
            wb_perf_unfire(tmp, e->deck);
            break;
        case WB_PERF_FADE:
            wb_perf_set_clock(tmp, e->t);
            wb_perf_fade(tmp, e->v);
            break;
        case WB_PERF_PARAM:
            wb_perf_set_clock(tmp, e->t);
            wb_perf_param(tmp, e->deck, e->which, e->v);
            break;
        default: break;
        }
    }
    /* NOTE: firing while recording would append to tmp's log too — but we
     * never armed recording on tmp, so its log stays empty; the replay
     * above mutated deck state directly. Render the state as-is. */
    wb_perf_render_frame(tmp, rgba);
    wb_perf_free(tmp);
    return 0;
}

int wb_perfclip_render_seq(const wb_perfclip *pc,
                           double t0, double dur, double fps,
                           int w, int h, const char *raw_path) {
    if (!pc || !raw_path || dur <= 0 || fps <= 0 || w <= 0 || h <= 0)
        return -1;
    FILE *f = fopen(raw_path, "wb");
    if (!f) return -1;
    int frames = (int)(dur * fps);
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) { fclose(f); return -1; }
    for (int i = 0; i < frames; i++) {
        double t = t0 + (double)i / fps;
        if (wb_perfclip_render((wb_perfclip *)pc, t, rgba, w, h) != 0) {
            free(rgba); fclose(f); return -1;
        }
        fwrite(rgba, 1, (size_t)w * h * 4, f);
    }
    free(rgba);
    fclose(f);
    return frames > 0 ? 0 : -1;
}

int wb_perfclip_save(const wb_perfclip *pc, const char *path) {
    if (!pc || !path) return -1;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f,
        "{\n"
        "  \"type\": \"perfclip\",\n"
        "  \"start\": %.6f,\n"
        "  \"duration\": %.6f,\n"
        "  \"decks\": %d,\n"
        "  \"events\": %u\n"
        "}\n",
        pc->start, pc->duration, pc->deck_count, pc->event_count);
    fclose(f);
    return wb_shadowbin_atomic_commit(tmp, path);
}
