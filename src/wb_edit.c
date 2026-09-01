/* wb_edit.c — Video edit decision list (EDL) model (R084).
 *
 * Implements the edit graph: tracks, clips, transitions, and the
 * timeline→compositor mapping. Evaluation at time T finds active clips,
 * pulls their source+FX subgraphs, and composites the results.
 */

#include "wbus/wbus_edit.h"
#include "wbus/wbus_video.h"
#include "wbus/wbus_compositor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---- edit graph lifecycle --------------------------------------------- */

wb_edit_graph *wb_edit_graph_create(double fps, int w, int h) {
    wb_edit_graph *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->fps = fps > 0 ? fps : 30.0;
    g->width = w > 0 ? w : 854;
    g->height = h > 0 ? h : 480;
    g->track_cap = 16;
    g->tracks = calloc(g->track_cap, sizeof(wb_edit_track));
    if (!g->tracks) { free(g); return NULL; }
    g->eval_time = -1.0;

    /* Create the output composite node */
    g->output_composite = wb_node_composite();
    if (!g->output_composite) {
        free(g->tracks); free(g); return NULL;
    }

    return g;
}

void wb_edit_graph_destroy(wb_edit_graph *g) {
    if (!g) return;

    /* Destroy all clips' node graphs */
    for (uint32_t t = 0; t < g->track_count; t++) {
        wb_edit_track *tr = &g->tracks[t];
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            wb_edit_clip *cl = &tr->clips[c];
            if (cl->fx_chain) wb_node_destroy(cl->fx_chain);
            if (cl->source_node) wb_node_destroy(cl->source_node);
        }
        free(tr->clips);
        free(tr->transitions);
    }
    free(g->tracks);

    /* Destroy output composite (owns its inputs) */
    if (g->output_composite) wb_node_destroy(g->output_composite);

    if (g->eval_frame) wb_frame_free(g->eval_frame);

    free(g);
}

/* ---- track management -------------------------------------------------- */

int wb_edit_add_track(wb_edit_graph *g, const char *name) {
    if (!g || !name) return -1;
    if (g->track_count >= g->track_cap) {
        uint32_t new_cap = g->track_cap * 2;
        wb_edit_track *new_tracks = realloc(g->tracks, new_cap * sizeof(wb_edit_track));
        if (!new_tracks) return -1;
        memset(new_tracks + g->track_cap, 0, (new_cap - g->track_cap) * sizeof(wb_edit_track));
        g->tracks = new_tracks;
        g->track_cap = new_cap;
    }
    int idx = (int)g->track_count;
    wb_edit_track *tr = &g->tracks[idx];
    snprintf(tr->name, sizeof(tr->name), "%s", name);
    tr->clip_cap = 16;
    tr->clips = calloc(tr->clip_cap, sizeof(wb_edit_clip));
    tr->trans_cap = 16;
    tr->transitions = calloc(tr->trans_cap, sizeof(wb_edit_transition));
    tr->volume = 1.0f;
    g->track_count++;
    return idx;
}

void wb_edit_remove_track(wb_edit_graph *g, int track_idx) {
    if (!g || track_idx < 0 || (uint32_t)track_idx >= g->track_count) return;
    wb_edit_track *tr = &g->tracks[track_idx];
    /* Destroy clip node graphs */
    for (uint32_t c = 0; c < tr->clip_count; c++) {
        wb_edit_clip *cl = &tr->clips[c];
        if (cl->fx_chain) wb_node_destroy(cl->fx_chain);
        if (cl->source_node) wb_node_destroy(cl->source_node);
    }
    free(tr->clips);
    free(tr->transitions);
    /* Shift tracks down */
    memmove(&g->tracks[track_idx], &g->tracks[track_idx + 1],
            (g->track_count - track_idx - 1) * sizeof(wb_edit_track));
    g->track_count--;
}

/* ---- clip management --------------------------------------------------- */

int wb_edit_add_clip(wb_edit_graph *g, int track,
                      const char *source_path,
                      double start_in_source,
                      double duration,
                      double timeline_pos) {
    if (!g || !source_path || track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    if (tr->clip_count >= tr->clip_cap) {
        uint32_t new_cap = tr->clip_cap * 2;
        wb_edit_clip *new_clips = realloc(tr->clips, new_cap * sizeof(wb_edit_clip));
        if (!new_clips) return -1;
        memset(new_clips + tr->clip_cap, 0, (new_cap - tr->clip_cap) * sizeof(wb_edit_clip));
        tr->clips = new_clips;
        tr->clip_cap = new_cap;
    }
    int idx = (int)tr->clip_count;
    wb_edit_clip *cl = &tr->clips[idx];
    snprintf(cl->source_path, sizeof(cl->source_path), "%s", source_path);
    cl->start_in_source = start_in_source;
    cl->duration = duration;
    cl->timeline_pos = timeline_pos;
    cl->track = track;
    cl->speed = 1.0f;
    cl->gain = 1.0f;
    cl->fx_chain = NULL;
    cl->source_node = NULL;

    /* Create the video source node for this clip */
    cl->source_node = wb_node_source_video(source_path, g->width, g->height);
    if (!cl->source_node) {
        fprintf(stderr, "wb_edit: failed to create video source for %s\n", source_path);
        return -1;
    }

    tr->clip_count++;

    /* Update timeline duration */
    double clip_end = timeline_pos + duration;
    if (clip_end > g->duration) g->duration = clip_end;

    return idx;
}

void wb_edit_remove_clip(wb_edit_graph *g, int track, int clip_idx) {
    if (!g || track < 0 || (uint32_t)track >= g->track_count) return;
    wb_edit_track *tr = &g->tracks[track];
    if (clip_idx < 0 || (uint32_t)clip_idx >= tr->clip_count) return;
    wb_edit_clip *cl = &tr->clips[clip_idx];
    if (cl->fx_chain) wb_node_destroy(cl->fx_chain);
    if (cl->source_node) wb_node_destroy(cl->source_node);
    memmove(&tr->clips[clip_idx], &tr->clips[clip_idx + 1],
            (tr->clip_count - clip_idx - 1) * sizeof(wb_edit_clip));
    tr->clip_count--;
}

int wb_edit_move_clip(wb_edit_graph *g, int track, int clip_idx,
                       double new_timeline_pos) {
    if (!g || track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    if (clip_idx < 0 || (uint32_t)clip_idx >= tr->clip_count) return -1;
    tr->clips[clip_idx].timeline_pos = new_timeline_pos;
    return 0;
}

int wb_edit_split_clip(wb_edit_graph *g, int track, int clip_idx,
                        double split_pos) {
    if (!g || track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    if (clip_idx < 0 || (uint32_t)clip_idx >= tr->clip_count) return -1;
    wb_edit_clip *cl = &tr->clips[clip_idx];

    /* split_pos is in timeline seconds */
    double local_t = split_pos - cl->timeline_pos;
    if (local_t <= 0 || local_t >= cl->duration) return -1;

    /* Add a new clip after this one */
    int new_idx = wb_edit_add_clip(g, track, cl->source_path,
                                    cl->start_in_source + local_t,
                                    cl->duration - local_t,
                                    split_pos);
    if (new_idx < 0) return -1;

    /* Truncate the original clip */
    cl->duration = local_t;

    return new_idx;
}

/* ---- transitions ------------------------------------------------------- */

int wb_edit_add_transition(wb_edit_graph *g, int track,
                            int clip_a_idx,
                            wb_edit_trans_type type,
                            double duration) {
    if (!g || track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    if (clip_a_idx < 0 || (uint32_t)clip_a_idx + 1 >= tr->clip_count) return -1;

    if (tr->trans_count >= tr->trans_cap) {
        uint32_t new_cap = tr->trans_cap * 2;
        wb_edit_transition *new_trans = realloc(tr->transitions, new_cap * sizeof(wb_edit_transition));
        if (!new_trans) return -1;
        memset(new_trans + tr->trans_cap, 0, (new_cap - tr->trans_cap) * sizeof(wb_edit_transition));
        tr->transitions = new_trans;
        tr->trans_cap = new_cap;
    }

    int idx = (int)tr->trans_count;
    wb_edit_transition *trn = &tr->transitions[idx];
    trn->type = type;
    trn->duration = duration;
    trn->clip_a_idx = clip_a_idx;
    trn->clip_b_idx = clip_a_idx + 1;

    /* Create the compositor transition node */
    int trans_op = 0; /* crossfade */
    if (type == WB_EDIT_TRANS_DIP_TO_BLACK) trans_op = 1;
    trn->trans_node = wb_node_transition(trans_op, duration);

    tr->trans_count++;
    return idx;
}

/* ---- effects ----------------------------------------------------------- */

int wb_edit_clip_add_effect(wb_edit_graph *g, int track, int clip_idx,
                             wb_node *effect) {
    if (!g || !effect || track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    if (clip_idx < 0 || (uint32_t)clip_idx >= tr->clip_count) return -1;
    wb_edit_clip *cl = &tr->clips[clip_idx];

    if (!cl->fx_chain) {
        /* First effect: connect to source */
        cl->fx_chain = effect;
        wb_node_connect(effect, cl->source_node, 0);
    } else {
        /* Append to end of chain: find the last node */
        /* Simple approach: connect new effect to current chain head */
        wb_node_connect(effect, cl->fx_chain, 0);
        cl->fx_chain = effect;
    }

    return 0;
}

/* ---- evaluation -------------------------------------------------------- */

/* Find the clip active at a timeline position on a track */
int wb_edit_clip_at(wb_edit_graph *g, int track, double timeline_pos) {
    if (!g || track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    for (uint32_t c = 0; c < tr->clip_count; c++) {
        wb_edit_clip *cl = &tr->clips[c];
        if (timeline_pos >= cl->timeline_pos &&
            timeline_pos < cl->timeline_pos + cl->duration) {
            return (int)c;
        }
    }
    return -1;
}

wb_frame *wb_edit_graph_evaluate(wb_edit_graph *g, double time_sec) {
    if (!g) return NULL;

    /* Cache hit */
    if (g->eval_frame && fabs(time_sec - g->eval_time) < 0.001) {
        return wb_frame_ref(g->eval_frame);
    }

    /* Clear previous cache */
    if (g->eval_frame) {
        wb_frame_free(g->eval_frame);
        g->eval_frame = NULL;
    }
    g->eval_time = time_sec;

    /* Allocate output frame */
    wb_frame *out = wb_frame_alloc(g->width, g->height);
    if (!out) return NULL;
    out->roi_x = 0; out->roi_y = 0;
    out->roi_w = g->width; out->roi_h = g->height;

    /* Composite all active clips at this time */
    int active_count = 0;
    for (uint32_t t = 0; t < g->track_count; t++) {
        if (g->tracks[t].muted) continue;
        int ci = wb_edit_clip_at(g, (int)t, time_sec);
        if (ci < 0) continue;

        wb_edit_clip *cl = &g->tracks[t].clips[ci];

        /* Calculate source time */
        double local_t = time_sec - cl->timeline_pos;
        double source_t = cl->start_in_source + local_t * cl->speed;

        /* Pull from FX chain or source */
        wb_node *pull_root = cl->fx_chain ? cl->fx_chain : cl->source_node;
        if (!pull_root) continue;

        wb_frame *clip_frame = wb_node_pull(pull_root, source_t, 0, 0, g->width, g->height);
        if (!clip_frame) continue;

        /* Composite onto output (over operator) */
        for (int y = 0; y < g->height; y++) {
            for (int x = 0; x < g->width; x++) {
                int idx = y * g->width + x;
                wb_px *dst = &out->px[idx];
                wb_px *src = &clip_frame->px[idx];
                /* Alpha over */
                float sa = src->a;
                dst->r = src->r * sa + dst->r * (1.0f - sa);
                dst->g = src->g * sa + dst->g * (1.0f - sa);
                dst->b = src->b * sa + dst->b * (1.0f - sa);
                dst->a = src->a + dst->a * (1.0f - sa);
            }
        }
        wb_frame_free(clip_frame);
        active_count++;
    }

    if (active_count == 0) {
        /* No active clips: return black */
        memset(out->px, 0, g->width * g->height * sizeof(wb_px));
    }

    g->eval_frame = wb_frame_ref(out);
    return out;
}

/* ---- export ------------------------------------------------------------ */

int wb_edit_graph_render_to_mp4(wb_edit_graph *g, const char *out_path,
                                 volatile int *cancel,
                                 wb_export_prog_fn prog, void *prog_ctx) {
    if (!g || !out_path) return -1;
    return wb_compositor_render_to_mp4(NULL, out_path, g->fps,
                                        g->width, g->height,
                                        g->duration, cancel,
                                        prog, prog_ctx);
}

/* ---- query ------------------------------------------------------------- */

double wb_edit_graph_get_duration(const wb_edit_graph *g) {
    return g ? g->duration : 0.0;
}
