/* wb_edit.c — Video edit decision list (EDL) model (R084).
 *
 * Implements the edit graph: tracks, clips, transitions, and the
 * timeline→compositor mapping. Evaluation at time T finds active clips,
 * pulls their source+FX subgraphs, and composites the results.
 */

#include "wbus/wbus_edit.h"
#include "wbus/wbus_video.h"
#include "wbus/wbus_compositor.h"
#include "wbus/wb_internal.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
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

    /* Subtitle defaults */
    g->subtitle_text[0] = '\0';
    g->subtitle_pos_x = 0.05f;
    g->subtitle_pos_y = 0.85f;
    g->subtitle_size = 2.0f;
    g->subtitle_color = 0xFFFFFFFF;

    /* Proxy defaults */
    g->proxy_enabled = 0;
    g->proxy_w = 960;
    g->proxy_h = 540;
    strncpy(g->proxy_dir, "/tmp", sizeof(g->proxy_dir) - 1);

    /* Create the output composite node */
    g->output_composite = wb_node_composite();
    if (!g->output_composite) {
        free(g->tracks); free(g); return NULL;
    }

    /* ---- Color management post chain ----
     * composite -> colorspace -> tonemap -> output
     * Built at create() time; evaluate() pulls from post_output
     * when color_management_enabled, else from output_composite. */
    g->color_management_enabled = 0; /* off by default */
    g->input_cs  = WB_CS_SRGB_TO_LINEAR;
    g->output_cs = WB_CS_LINEAR_TO_SRGB;
    g->tonemap   = WB_TM_NONE;

    /* Colorspace node: applies input_cs then output_cs as a single pass.
     * We use input_cs as the node mode; the output_cs is applied by
     * wiring a second colorspace if input != output identity. For the
     * common sRGB->linear->...->linear->sRGB case we chain two nodes. */
    g->cs_node = wb_node_colorspace(g->input_cs);
    if (!g->cs_node) {
        wb_node_destroy(g->output_composite); free(g->tracks); free(g); return NULL;
    }

    /* Tonemap node */
    g->tm_node = wb_node_tonemap(g->tonemap);
    if (!g->tm_node) {
        wb_node_destroy(g->cs_node); wb_node_destroy(g->output_composite);
        free(g->tracks); free(g); return NULL;
    }

    /* Wire: composite -> cs_node -> tm_node */
    wb_node_connect(g->cs_node, g->output_composite, 0);
    wb_node_connect(g->tm_node, g->cs_node, 0);

    /* post_output is the pull endpoint of the chain */
    g->post_output = g->tm_node;

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
        free(tr->audio_clips);
    }
    free(g->tracks);

    /* Destroy post chain nodes (tm -> cs -> composite). Order matters:
     * destroy from the tail up so inputs are still valid during free. */
    if (g->tm_node) wb_node_destroy(g->tm_node);
    if (g->cs_node) wb_node_destroy(g->cs_node);

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
    tr->audio_clip_cap = 16;
    tr->audio_clips = calloc(tr->audio_clip_cap, sizeof(wb_edit_audio_clip));
    tr->audio_clip_count = 0;
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
    free(tr->audio_clips);
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

/* ---- scene-detection auto-cut ------------------------------------------ */

int wb_edit_auto_cut_scenes(wb_edit_graph *g, int track, int clip_idx,
                             float threshold) {
    if (!g || track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    if (clip_idx < 0 || (uint32_t)clip_idx >= tr->clip_count) return -1;
    wb_edit_clip *cl = &tr->clips[clip_idx];

    if (threshold <= 0.0f) threshold = 0.3f;
    if (threshold > 1.0f) threshold = 1.0f;

    /* Use wb_video_detect_segments() to find scene boundaries.
     * mode 0 = scene changes, threshold = sensitivity.
     * We allocate a reasonable number of segment slots. */
    #define MAX_SEG 256
    wb_video_segment segs[MAX_SEG];
    double thr = (double)threshold;
    int nseg = wb_video_detect_segments(cl->source_path, 0, thr, segs, MAX_SEG);
    if (nseg <= 0) {
        /* No scenes detected or error — return 0 cuts (not an error) */
        return 0;
    }

    /* Split the clip at each scene boundary that falls within the clip's
     * [timeline_pos, timeline_pos + duration) window. We must split from
     * the end backward so earlier splits don't shift later positions. */
    int cuts = 0;
    /* Collect valid split points (timeline seconds) */
    double split_points[MAX_SEG];
    int nsplit = 0;
    for (int i = 0; i < nseg && nsplit < MAX_SEG; i++) {
        /* segs[i].start is in source seconds; map to timeline */
        double src_boundary = segs[i].start;
        /* Only consider boundaries within the clip's source window */
        if (src_boundary <= cl->start_in_source ||
            src_boundary >= cl->start_in_source + cl->duration) {
            continue;
        }
        /* Convert source time to timeline time */
        double local_src = src_boundary - cl->start_in_source;
        double tl_pos = cl->timeline_pos + local_src;
        split_points[nsplit++] = tl_pos;
    }

    /* Sort split points descending so we split from the end first.
     * This keeps earlier split positions valid after each split. */
    for (int i = 0; i < nsplit - 1; i++) {
        for (int j = i + 1; j < nsplit; j++) {
            if (split_points[j] > split_points[i]) {
                double tmp = split_points[i];
                split_points[i] = split_points[j];
                split_points[j] = tmp;
            }
        }
    }

    /* Split at each point. After splitting, the original clip is truncated
     * and a new clip is inserted at the split position. We always split
     * clip_idx (the leftmost piece), since splits happen right-to-left. */
    for (int i = 0; i < nsplit; i++) {
        int new_idx = wb_edit_split_clip(g, track, clip_idx, split_points[i]);
        if (new_idx < 0) break;  /* Stop on error but keep cuts so far */
        cuts++;
    }

    return cuts;
}

/* ---- audio clip management --------------------------------------------- */

int wb_edit_add_audio_clip(wb_edit_graph *g, int track,
                             const char *source,
                             double start, double dur, double tl_pos) {
    if (!g || !source || track < 0 || (uint32_t)track >= g->track_count) return -1;
    if (dur <= 0) return -1;
    wb_edit_track *tr = &g->tracks[track];

    /* Grow array if needed */
    if (tr->audio_clip_count >= tr->audio_clip_cap) {
        uint32_t new_cap = tr->audio_clip_cap > 0 ? tr->audio_clip_cap * 2 : 8;
        wb_edit_audio_clip *new_clips = realloc(tr->audio_clips,
                                                  new_cap * sizeof(wb_edit_audio_clip));
        if (!new_clips) return -1;
        memset(new_clips + tr->audio_clip_cap, 0,
               (new_cap - tr->audio_clip_cap) * sizeof(wb_edit_audio_clip));
        tr->audio_clips = new_clips;
        tr->audio_clip_cap = new_cap;
    }

    int idx = (int)tr->audio_clip_count;
    wb_edit_audio_clip *ac = &tr->audio_clips[idx];
    snprintf(ac->source_path, sizeof(ac->source_path), "%s", source);
    ac->start_in_source = start;
    ac->duration = dur;
    ac->timeline_pos = tl_pos;
    ac->volume = 1.0f;
    ac->speed = 1.0f;

    tr->audio_clip_count++;

    /* Update timeline duration */
    double clip_end = tl_pos + dur;
    if (clip_end > g->duration) g->duration = clip_end;

    return idx;
}

int wb_edit_set_audio_volume(wb_edit_graph *g, int track, int clip_idx,
                               float vol) {
    if (!g || track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    if (clip_idx < 0 || (uint32_t)clip_idx >= tr->audio_clip_count) return -1;
    if (vol < 0.0f) vol = 0.0f;
    tr->audio_clips[clip_idx].volume = vol;
    return 0;
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

/* ---- color management pipeline ----------------------------------------- */

void wb_edit_set_color_management(wb_edit_graph *g, int enable) {
    if (!g) return;
    g->color_management_enabled = enable ? 1 : 0;
    /* Invalidate cache so next evaluate picks up the change */
    if (g->eval_frame) { wb_frame_free(g->eval_frame); g->eval_frame = NULL; }
}

void wb_edit_set_input_colorspace(wb_edit_graph *g, wb_cs_mode mode) {
    if (!g) return;
    g->input_cs = mode;
    /* Recreate the colorspace node with the new mode.
     * Rewire: cs_node input stays the same (output_composite). */
    if (g->cs_node) {
        /* Save the input node (output_composite) before destroying */
        wb_node *input_node = g->cs_node->inputs ? g->cs_node->inputs[0] : NULL;
        wb_node_destroy(g->cs_node);
        g->cs_node = wb_node_colorspace(mode);
        if (g->cs_node && input_node) {
            wb_node_connect(g->cs_node, input_node, 0);
        }
        /* Rewire tonemap to new cs_node */
        if (g->tm_node && g->cs_node) {
            wb_node_connect(g->tm_node, g->cs_node, 0);
        }
    }
    if (g->eval_frame) { wb_frame_free(g->eval_frame); g->eval_frame = NULL; }
}

void wb_edit_set_output_colorspace(wb_edit_graph *g, wb_cs_mode mode) {
    if (!g) return;
    g->output_cs = mode;
    /* The output_cs is tracked for the pipeline; in this single-CST-node
     * model the node applies input_cs. A full two-CST pipeline would
     * rewire here. For now we store it for the API contract and future
     * dual-node chain. */
    if (g->eval_frame) { wb_frame_free(g->eval_frame); g->eval_frame = NULL; }
}

void wb_edit_set_tonemap(wb_edit_graph *g, wb_tm_op op) {
    if (!g) return;
    g->tonemap = op;
    /* Recreate the tonemap node with the new operator.
     * Rewire: tm_node input stays the same (cs_node). */
    if (g->tm_node) {
        wb_node *input_node = g->tm_node->inputs ? g->tm_node->inputs[0] : NULL;
        wb_node_destroy(g->tm_node);
        g->tm_node = wb_node_tonemap(op);
        if (g->tm_node && input_node) {
            wb_node_connect(g->tm_node, input_node, 0);
        }
    }
    /* Update post_output pointer (it's the chain endpoint) */
    g->post_output = g->tm_node;
    if (g->eval_frame) { wb_frame_free(g->eval_frame); g->eval_frame = NULL; }
}

/* ---- subtitle burn-in -------------------------------------------------- */

void wb_edit_set_subtitle(wb_edit_graph *g, const char *text) {
    if (!g) return;
    if (text && text[0]) {
        snprintf(g->subtitle_text, sizeof(g->subtitle_text), "%s", text);
    } else {
        g->subtitle_text[0] = '\0';
    }
}

void wb_edit_set_subtitle_position(wb_edit_graph *g, float x, float y) {
    if (!g) return;
    g->subtitle_pos_x = x < 0 ? 0 : (x > 1 ? 1 : x);
    g->subtitle_pos_y = y < 0 ? 0 : (y > 1 ? 1 : y);
}

void wb_edit_set_subtitle_size(wb_edit_graph *g, float size) {
    if (!g) return;
    g->subtitle_size = size > 0 ? size : 1.0f;
}

void wb_edit_set_subtitle_color(wb_edit_graph *g, uint32_t rgba) {
    if (!g) return;
    /* Input is 0xRRGGBB (24-bit hex). Store as 0xRRGGBBAA. */
    uint32_t r = (rgba >> 16) & 0xFF;
    uint32_t gg = (rgba >> 8) & 0xFF;
    uint32_t b = rgba & 0xFF;
    g->subtitle_color = (r << 24) | (gg << 16) | (b << 8) | 0xFF;
}

/* ---- 5x7 bitmap font (ASCII 32-127) ----------------------------------- */
/* Each byte is a column, 5 columns per character, 7 rows tall.
 * Bits are read top-to-bottom within each column byte (bit 6 = top row). */

#define FONT_W 5
#define FONT_H 7

static const uint8_t font_5x7[][5] = {
    /* space */ {0x00,0x00,0x00,0x00,0x00},
    /* ! */     {0x00,0x00,0x5F,0x00,0x00},
    /* " */     {0x00,0x07,0x00,0x07,0x00},
    /* # */     {0x14,0x7F,0x14,0x7F,0x14},
    /* $ */     {0x24,0x2A,0x7F,0x2A,0x12},
    /* % */     {0x23,0x13,0x08,0x64,0x62},
    /* & */     {0x36,0x49,0x55,0x22,0x50},
    /* ' */     {0x00,0x05,0x03,0x00,0x00},
    /* ( */     {0x00,0x1C,0x22,0x41,0x00},
    /* ) */     {0x00,0x41,0x22,0x1C,0x00},
    /* * */     {0x08,0x2A,0x1C,0x2A,0x08},
    /* + */     {0x08,0x08,0x3E,0x08,0x08},
    /* , */     {0x00,0x50,0x30,0x00,0x00},
    /* - */     {0x08,0x08,0x08,0x08,0x08},
    /* . */     {0x00,0x60,0x60,0x00,0x00},
    /* / */     {0x20,0x10,0x08,0x04,0x02},
    /* 0 */     {0x3E,0x51,0x49,0x45,0x3E},
    /* 1 */     {0x00,0x42,0x7F,0x40,0x00},
    /* 2 */     {0x42,0x61,0x51,0x49,0x46},
    /* 3 */     {0x21,0x41,0x45,0x4B,0x31},
    /* 4 */     {0x18,0x14,0x12,0x7F,0x10},
    /* 5 */     {0x27,0x45,0x45,0x45,0x39},
    /* 6 */     {0x3C,0x4A,0x49,0x49,0x30},
    /* 7 */     {0x01,0x71,0x09,0x05,0x03},
    /* 8 */     {0x36,0x49,0x49,0x49,0x36},
    /* 9 */     {0x06,0x49,0x49,0x29,0x1E},
    /* : */     {0x00,0x36,0x36,0x00,0x00},
    /* ; */     {0x00,0x56,0x36,0x00,0x00},
    /* < */     {0x00,0x08,0x14,0x22,0x41},
    /* = */     {0x14,0x14,0x14,0x14,0x14},
    /* > */     {0x41,0x22,0x14,0x08,0x00},
    /* ? */     {0x02,0x01,0x51,0x09,0x06},
    /* @ */     {0x32,0x49,0x79,0x41,0x3E},
    /* A */     {0x7E,0x11,0x11,0x11,0x7E},
    /* B */     {0x7F,0x49,0x49,0x49,0x36},
    /* C */     {0x3E,0x41,0x41,0x41,0x22},
    /* D */     {0x7F,0x41,0x41,0x22,0x1C},
    /* E */     {0x7F,0x49,0x49,0x49,0x41},
    /* F */     {0x7F,0x09,0x09,0x01,0x01},
    /* G */     {0x3E,0x41,0x41,0x51,0x32},
    /* H */     {0x7F,0x08,0x08,0x08,0x7F},
    /* I */     {0x00,0x41,0x7F,0x41,0x00},
    /* J */     {0x20,0x40,0x41,0x3F,0x01},
    /* K */     {0x7F,0x08,0x14,0x22,0x41},
    /* L */     {0x7F,0x40,0x40,0x40,0x40},
    /* M */     {0x7F,0x02,0x04,0x02,0x7F},
    /* N */     {0x7F,0x04,0x08,0x10,0x7F},
    /* O */     {0x3E,0x41,0x41,0x41,0x3E},
    /* P */     {0x7F,0x09,0x09,0x09,0x06},
    /* Q */     {0x3E,0x41,0x51,0x21,0x5E},
    /* R */     {0x7F,0x09,0x19,0x29,0x46},
    /* S */     {0x46,0x49,0x49,0x49,0x31},
    /* T */     {0x01,0x01,0x7F,0x01,0x01},
    /* U */     {0x3F,0x40,0x40,0x40,0x3F},
    /* V */     {0x1F,0x20,0x40,0x20,0x1F},
    /* W */     {0x7F,0x20,0x18,0x20,0x7F},
    /* X */     {0x63,0x14,0x08,0x14,0x63},
    /* Y */     {0x03,0x04,0x78,0x04,0x03},
    /* Z */     {0x61,0x51,0x49,0x45,0x43},
    /* [ */     {0x00,0x00,0x7F,0x41,0x41},
    /* \ */     {0x02,0x04,0x08,0x10,0x20},
    /* ] */     {0x41,0x41,0x7F,0x00,0x00},
    /* ^ */     {0x04,0x02,0x01,0x02,0x04},
    /* _ */     {0x40,0x40,0x40,0x40,0x40},
    /* ` */     {0x00,0x01,0x02,0x04,0x00},
    /* a */     {0x20,0x54,0x54,0x54,0x78},
    /* b */     {0x7F,0x48,0x44,0x44,0x38},
    /* c */     {0x38,0x44,0x44,0x44,0x20},
    /* d */     {0x38,0x44,0x44,0x48,0x7F},
    /* e */     {0x38,0x54,0x54,0x54,0x18},
    /* f */     {0x08,0x7E,0x09,0x01,0x02},
    /* g */     {0x08,0x14,0x54,0x54,0x3C},
    /* h */     {0x7F,0x08,0x04,0x04,0x78},
    /* i */     {0x00,0x44,0x7D,0x40,0x00},
    /* j */     {0x20,0x40,0x44,0x3D,0x00},
    /* k */     {0x00,0x7F,0x10,0x28,0x44},
    /* l */     {0x00,0x41,0x7F,0x40,0x00},
    /* m */     {0x7C,0x04,0x18,0x04,0x78},
    /* n */     {0x7C,0x08,0x04,0x04,0x78},
    /* o */     {0x38,0x44,0x44,0x44,0x38},
    /* p */     {0x7C,0x14,0x14,0x14,0x08},
    /* q */     {0x08,0x14,0x14,0x18,0x7C},
    /* r */     {0x7C,0x08,0x04,0x04,0x08},
    /* s */     {0x48,0x54,0x54,0x54,0x20},
    /* t */     {0x04,0x3F,0x44,0x40,0x20},
    /* u */     {0x3C,0x40,0x40,0x20,0x7C},
    /* v */     {0x1C,0x20,0x40,0x20,0x1C},
    /* w */     {0x3C,0x40,0x30,0x40,0x3C},
    /* x */     {0x44,0x28,0x10,0x28,0x44},
    /* y */     {0x0C,0x50,0x50,0x50,0x3C},
    /* z */     {0x44,0x64,0x54,0x4C,0x44},
    /* { */     {0x00,0x08,0x36,0x41,0x00},
    /* | */     {0x00,0x00,0x7F,0x00,0x00},
    /* } */     {0x00,0x41,0x36,0x08,0x00},
    /* ~ */     {0x08,0x04,0x08,0x10,0x08},
};

/* Render text into a uint8 RGBA buffer with outline for readability.
 * Draws at pixel (origin_x, origin_y). scale is integer pixel size per font pixel.
 * color is 0xRRGGBBAA. */
static void subtitle_render_text(uint8_t *rgba, int w, int h,
                                  int origin_x, int origin_y,
                                  const char *text, int scale, uint32_t color) {
    uint8_t cr = (color >> 24) & 0xFF;
    uint8_t cg = (color >> 16) & 0xFF;
    uint8_t cb = (color >> 8) & 0xFF;
    uint8_t ca = color & 0xFF;

    int cursor_x = origin_x;
    int line_len = 0;

    for (const char *c = text; *c; c++) {
        /* Word wrap: if next word would exceed width, wrap */
        if (*c == '\n' || *c == '\r') {
            cursor_x = origin_x;
            origin_y += (FONT_H + 2) * scale;
            line_len = 0;
            continue;
        }
        if (*c == ' ') {
            /* Check if next word fits */
            int next_word_end = cursor_x;
            const char *wp = c + 1;
            while (*wp && *wp != ' ' && *wp != '\n') {
                next_word_end += (FONT_W + 1) * scale;
                wp++;
            }
            if (next_word_end > w - scale) {
                cursor_x = origin_x;
                origin_y += (FONT_H + 2) * scale;
                line_len = 0;
                continue;
            }
        }

        unsigned char ch = (unsigned char)*c;
        if (ch < 32 || ch > 127) ch = '?';
        const uint8_t *glyph = font_5x7[ch - 32];

        /* Draw outline first (black shadow offset by 1px in each direction) */
        for (int col = 0; col < FONT_W; col++) {
            uint8_t column_bits = glyph[col];
            for (int row = 0; row < FONT_H; row++) {
                if (!(column_bits & (1 << (6 - row)))) continue;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        /* Outline: draw at offsets -1 and +1 */
                        for (int dy = -1; dy <= 1; dy += 2) {
                            for (int dx = -1; dx <= 1; dx += 2) {
                                int px = cursor_x + col * scale + sx + dx;
                                int py = origin_y + row * scale + sy + dy;
                                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                                int idx = (py * w + px) * 4;
                                rgba[idx+0] = 0x00;
                                rgba[idx+1] = 0x00;
                                rgba[idx+2] = 0x00;
                                rgba[idx+3] = 0xFF;
                            }
                        }
                        /* Foreground pixel */
                        int px = cursor_x + col * scale + sx;
                        int py = origin_y + row * scale + sy;
                        if (px < 0 || px >= w || py < 0 || py >= h) continue;
                        int idx = (py * w + px) * 4;
                        /* Blend foreground over whatever is there */
                        float fg_a = ca / 255.0f;
                        rgba[idx+0] = (uint8_t)(cr * fg_a + rgba[idx+0] * (1.0f - fg_a));
                        rgba[idx+1] = (uint8_t)(cg * fg_a + rgba[idx+1] * (1.0f - fg_a));
                        rgba[idx+2] = (uint8_t)(cb * fg_a + rgba[idx+2] * (1.0f - fg_a));
                        rgba[idx+3] = 0xFF;
                    }
                }
            }
        }
        cursor_x += (FONT_W + 1) * scale;
        line_len++;
        /* Hard wrap if text exceeds frame width */
        if (cursor_x + (FONT_W + 1) * scale > w) {
            cursor_x = origin_x;
            origin_y += (FONT_H + 2) * scale;
            line_len = 0;
        }
    }
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

    /* Apply color management post chain if enabled.
     * Pull from post_output (tm_node) which is wired:
     *   composite -> colorspace -> tonemap -> output
     * We feed the composited frame into the composite node's inputs
     * by pulling from the post chain with the composited result.
     *
     * Since the post chain is already wired to the composite, we
     * connect the composite as the source feeding into cs_node.
     * The composite node itself has no inputs yet (clips are composited
     * manually above), so we use a different approach: pull from the
     * post chain by wrapping the composited frame as a source node. */
    if (g->color_management_enabled && g->post_output) {
        /* Create a source node wrapping the composited frame.
         * We use wb_node_source_frame to wrap the RGBA buffer. */
        /* Convert float RGBA to uint8 for source_frame node */
        uint8_t *rgba = malloc(g->width * g->height * 4);
        if (rgba) {
            for (int i = 0; i < g->width * g->height; i++) {
                rgba[i*4+0] = (uint8_t)(out->px[i].r * 255.0f + 0.5f);
                rgba[i*4+1] = (uint8_t)(out->px[i].g * 255.0f + 0.5f);
                rgba[i*4+2] = (uint8_t)(out->px[i].b * 255.0f + 0.5f);
                rgba[i*4+3] = (uint8_t)(out->px[i].a * 255.0f + 0.5f);
            }
            wb_node *tmp_src = wb_node_source_frame(g->width, g->height, rgba);
            if (tmp_src) {
                /* Rewire: cs_node takes tmp_src as input */
                wb_node_connect(g->cs_node, tmp_src, 0);
                /* Pull from post_output (the chain endpoint) */
                wb_frame *graded = wb_node_pull(g->post_output, time_sec, 0, 0, g->width, g->height);
                if (graded) {
                    wb_frame_free(out);
                    out = graded;
                }
                /* Restore original wiring: cs_node takes output_composite */
                wb_node_connect(g->cs_node, g->output_composite, 0);
                wb_node_destroy(tmp_src);
            }
            free(rgba);
        }
    }

    g->eval_frame = wb_frame_ref(out);
    return out;
}

/* ---- export ------------------------------------------------------------ */

int wb_edit_graph_render_to_mp4(wb_edit_graph *g, const char *out_path,
                                 volatile int *cancel,
                                 wb_export_prog_fn prog, void *prog_ctx) {
    if (!g || !out_path) return -1;
    if (g->duration <= 0) return -1;

    /* Use the compositor encoder with a per-frame evaluate wrapper.
     * We create a fake root node that evaluates the edit graph on pull. */
    /* For now, use the direct render loop here since we need per-frame eval. */
    return wb_edit_render_to_mp4(g, out_path, cancel, prog, prog_ctx);
}

/* ---- query ------------------------------------------------------------- */

double wb_edit_graph_get_duration(const wb_edit_graph *g) {
    return g ? g->duration : 0.0;
}

/* ---- nested sequence --------------------------------------------------- */

/* Sequence source node state: on pull(time), evaluate the inner graph. */
typedef struct {
    wb_edit_graph *graph;
    int w, h;
} seq_source_state;

static wb_frame *seq_source_pull(wb_node *self, double t,
                                  int rx, int ry, int rw, int rh, int phase) {
    (void)phase;
    seq_source_state *st = self->user;
    if (!st || !st->graph) return NULL;

    /* Evaluate the inner edit graph at time t */
    wb_frame *f = wb_edit_graph_evaluate(st->graph, t);
    if (!f) return NULL;

    /* Clip requested ROI to frame bounds */
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx + rw > f->w) rw = f->w - rx;
    if (ry + rh > f->h) rh = f->h - ry;
    if (rw <= 0 || rh <= 0) {
        wb_frame_free(f);
        return NULL;
    }

    f->roi_x = rx;
    f->roi_y = ry;
    f->roi_w = rw;
    f->roi_h = rh;
    return f;
}

static void seq_source_free(wb_node *self) {
    seq_source_state *st = self->user;
    if (st) {
        /* Note: the graph is owned by wb_edit_sequence, not the node */
        free(st);
    }
}

wb_edit_sequence *wb_edit_sequence_create(double fps, int w, int h) {
    wb_edit_sequence *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->graph = wb_edit_graph_create(fps, w, h);
    if (!s->graph) {
        free(s);
        return NULL;
    }

    /* Create the source node that evaluates the inner graph on pull */
    wb_node *node = wb_node_create(WB_NODE_SOURCE, "seq_source");
    if (!node) {
        wb_edit_graph_destroy(s->graph);
        free(s);
        return NULL;
    }

    seq_source_state *st = calloc(1, sizeof(*st));
    if (!st) {
        wb_node_destroy(node);
        wb_edit_graph_destroy(s->graph);
        free(s);
        return NULL;
    }
    st->graph = s->graph;
    st->w = s->graph->width;
    st->h = s->graph->height;

    node->user = st;
    node->pull = seq_source_pull;
    node->free = seq_source_free;
    wb_node_set_format(node, st->w, st->h);

    s->source_node = node;
    s->duration = 0.0;
    return s;
}

void wb_edit_sequence_destroy(wb_edit_sequence *s) {
    if (!s) return;
    if (s->source_node) wb_node_destroy(s->source_node);
    if (s->graph) wb_edit_graph_destroy(s->graph);
    free(s);
}

wb_edit_graph *wb_edit_sequence_graph(wb_edit_sequence *s) {
    return s ? s->graph : NULL;
}

wb_node *wb_edit_sequence_node(wb_edit_sequence *s) {
    return s ? s->source_node : NULL;
}

/* ---- direct render to MP4 (evaluates edit graph per frame) ------------- */

int wb_edit_render_to_mp4(wb_edit_graph *g, const char *out_path,
                           volatile int *cancel,
                           wb_export_prog_fn prog, void *prog_ctx) {
    if (!g || !out_path || g->duration <= 0) return -1;

    int ret = 0;
    int w = g->width, h = g->height;
    double fps = g->fps;
    int64_t total_frames = (int64_t)(g->duration * fps);

    /* Set up libav encoder */
    AVFormatContext *fmt_ctx = NULL;
    avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, out_path);
    if (!fmt_ctx) { fprintf(stderr, "edit_render: alloc output failed\n"); return -1; }

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) { fprintf(stderr, "edit_render: H.264 not found\n"); ret = -1; goto cleanup; }

    AVStream *stream = avformat_new_stream(fmt_ctx, codec);
    if (!stream) { fprintf(stderr, "edit_render: new stream failed\n"); ret = -1; goto cleanup; }

    AVCodecContext *enc_ctx = avcodec_alloc_context3(codec);
    if (!enc_ctx) { fprintf(stderr, "edit_render: alloc enc ctx failed\n"); ret = -1; goto cleanup; }

    enc_ctx->width = w;
    enc_ctx->height = h;
    enc_ctx->time_base = (AVRational){1, (int)fps};
    enc_ctx->framerate = (AVRational){(int)fps, 1};
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->bit_rate = 2000000;
    if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "preset", "veryfast", 0);
    av_dict_set(&opts, "crf", "23", 0);

    if (avcodec_open2(enc_ctx, codec, &opts) < 0) {
        fprintf(stderr, "edit_render: codec open failed\n"); ret = -1; goto enc_cleanup;
    }
    av_dict_free(&opts);

    avcodec_parameters_from_context(stream->codecpar, enc_ctx);
    stream->time_base = enc_ctx->time_base;

    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt_ctx->pb, out_path, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "edit_render: avio_open failed\n"); ret = -1; goto enc_cleanup;
        }
    }
    if (avformat_write_header(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "edit_render: write header failed\n"); ret = -1; goto enc_cleanup;
    }

    /* SwsContext: RGBA uint8 → YUV420P */
    struct SwsContext *sws = sws_getContext(w, h, AV_PIX_FMT_RGBA,
                                              w, h, AV_PIX_FMT_YUV420P,
                                              SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) { fprintf(stderr, "edit_render: sws failed\n"); ret = -1; goto enc_cleanup; }

    AVFrame *yuv_frame = av_frame_alloc();
    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = w;
    yuv_frame->height = h;
    av_frame_get_buffer(yuv_frame, 0);

    AVPacket *pkt = av_packet_alloc();

    int64_t frame_idx = 0;
    for (double t = 0.0; t < g->duration; t += 1.0 / fps) {
        if (cancel && *cancel) { ret = -2; break; }

        /* Evaluate edit graph at time t */
        wb_frame *f = wb_edit_graph_evaluate(g, t);
        if (!f) { fprintf(stderr, "edit_render: eval failed at t=%.3f\n", t); ret = -1; break; }

        /* Convert float RGBA → uint8 RGBA temp buffer */
        uint8_t *rgba = malloc(w * h * 4);
        for (int i = 0; i < w * h; i++) {
            rgba[i*4+0] = (uint8_t)(f->px[i].r * 255.0f + 0.5f);
            rgba[i*4+1] = (uint8_t)(f->px[i].g * 255.0f + 0.5f);
            rgba[i*4+2] = (uint8_t)(f->px[i].b * 255.0f + 0.5f);
            rgba[i*4+3] = (uint8_t)(f->px[i].a * 255.0f + 0.5f);
        }
        wb_frame_free(f);

        /* Subtitle burn-in: render text onto the RGBA frame */
        if (g->subtitle_text[0]) {
            int scale = (int)g->subtitle_size;
            if (scale < 1) scale = 1;
            int ox = (int)(g->subtitle_pos_x * w);
            int oy = (int)(g->subtitle_pos_y * h);
            subtitle_render_text(rgba, w, h, ox, oy,
                                 g->subtitle_text, scale, g->subtitle_color);
        }

        /* Scale to YUV420P */
        const uint8_t *src_slices[4] = { rgba, NULL, NULL, NULL };
        int src_strides[4] = { w * 4, 0, 0, 0 };
        sws_scale(sws, src_slices, src_strides, 0, h,
                  yuv_frame->data, yuv_frame->linesize);
        free(rgba);

        yuv_frame->pts = frame_idx;

        /* Encode */
        int got_packet = 0;
        if (avcodec_send_frame(enc_ctx, yuv_frame) == 0) {
            while (avcodec_receive_packet(enc_ctx, pkt) == 0) {
                av_packet_rescale_ts(pkt, enc_ctx->time_base, stream->time_base);
                pkt->stream_index = stream->index;
                av_interleaved_write_frame(fmt_ctx, pkt);
                av_packet_unref(pkt);
                got_packet = 1;
            }
        }

        frame_idx++;
        if (prog) prog(prog_ctx, t / g->duration);
    }

    /* Flush encoder */
    avcodec_send_frame(enc_ctx, NULL);
    while (avcodec_receive_packet(enc_ctx, pkt) == 0) {
        av_packet_rescale_ts(pkt, enc_ctx->time_base, stream->time_base);
        pkt->stream_index = stream->index;
        av_interleaved_write_frame(fmt_ctx, pkt);
        av_packet_unref(pkt);
    }

    av_write_trailer(fmt_ctx);
    if (prog) prog(prog_ctx, 1.0);

    av_packet_free(&pkt);
    av_frame_free(&yuv_frame);
    sws_freeContext(sws);

enc_cleanup:
    avcodec_free_context(&enc_ctx);
cleanup:
    if (fmt_ctx && !(fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&fmt_ctx->pb);
    avformat_free_context(fmt_ctx);
    return ret;
}

/* ---- proxy editing ------------------------------------------------------ */

void wb_edit_set_proxy_enabled(wb_edit_graph *g, int enable) {
    if (!g) return;
    g->proxy_enabled = enable;
    g->eval_time = -1.0;
}

void wb_edit_set_proxy_size(wb_edit_graph *g, int w, int h) {
    if (!g) return;
    g->proxy_w = w > 0 ? w : 960;
    g->proxy_h = h > 0 ? h : 540;
}

char *wb_edit_generate_proxy(wb_edit_graph *g, const char *source_path) {
    if (!g || !source_path) return NULL;
    char *proxy_path = malloc(512);
    const char *filename = strrchr(source_path, '/');
    filename = filename ? filename + 1 : source_path;
    snprintf(proxy_path, 512, "%s/proxy_%s_%dx%d.mp4",
             g->proxy_dir[0] ? g->proxy_dir : "/tmp",
             filename, g->proxy_w, g->proxy_h);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -i \"%s\" -vf \"scale=%d:%d\" -c:v libx264 "
             "-preset ultrafast -crf 28 -an \"%s\" >/dev/null 2>&1",
             source_path, g->proxy_w, g->proxy_h, proxy_path);
    int rc = system(cmd);
    if (rc != 0) { free(proxy_path); return NULL; }
    return proxy_path;
}

/* ---- keyframe animation ------------------------------------------------ */

/* Helper: get the Nth FX node from a clip's FX chain.
 * fx_chain is the last node; we traverse inputs to find earlier ones.
 * Returns NULL if not found. */
static wb_node *get_fx_node(wb_node *fx_chain, int fx_idx) {
    if (!fx_chain) return NULL;
    /* Collect nodes in order from source to output */
    wb_node *nodes[32];
    int count = 0;
    /* Walk from the output node back to the source */
    wb_node *cur = fx_chain;
    while (cur && count < 32) {
        nodes[count++] = cur;
        if (cur->n_inputs > 0 && cur->inputs[0] &&
            (cur->inputs[0]->kind == WB_NODE_EFFECT ||
             cur->inputs[0]->kind == WB_NODE_SOURCE)) {
            cur = cur->inputs[0];
        } else {
            break;
        }
    }
    /* nodes[0] is the last FX, nodes[count-1] is the first */
    int idx = count - 1 - fx_idx;
    if (idx < 0 || idx >= count) return NULL;
    return nodes[idx];
}

int wb_edit_set_keyframe(wb_edit_graph *g, int track, int clip_idx, int fx,
                          const char *param_name, double time, float value) {
    if (!g || !param_name) return -1;
    if (track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    if ((uint32_t)clip_idx >= tr->clip_count) return -1;
    wb_edit_clip *cl = &tr->clips[clip_idx];
    if (!cl->fx_chain) return -1;

    wb_node *fx_node = get_fx_node(cl->fx_chain, fx);
    if (!fx_node) return -1;

    /* Find or create the param track */
    wb_param_track *pt = NULL;
    int param_slot = -1;
    for (int i = 0; i < fx_node->n_params; i++) {
        if (strcmp(fx_node->param_names[i], param_name) == 0) {
            pt = fx_node->params[i];
            param_slot = i;
            break;
        }
    }

    if (!pt) {
        /* Create a new param track and add it to the node */
        pt = wb_param_track_create();
        if (!pt) return -1;
        param_slot = wb_node_add_param(fx_node, param_name, pt);
        if (param_slot < 0) { wb_param_track_free(pt); return -1; }
    }

    /* Add the keyframe */
    wb_param_track_set(pt, time, value, WB_KF_LINEAR);
    return 0;
}

float wb_edit_get_keyframed_value(wb_edit_graph *g, int track, int clip_idx,
                                   int fx, const char *param_name, double time) {
    if (!g || !param_name) return 0.0f;
    if (track < 0 || (uint32_t)track >= g->track_count) return 0.0f;
    wb_edit_track *tr = &g->tracks[track];
    if ((uint32_t)clip_idx >= tr->clip_count) return 0.0f;
    wb_edit_clip *cl = &tr->clips[clip_idx];
    if (!cl->fx_chain) return 0.0f;

    wb_node *fx_node = get_fx_node(cl->fx_chain, fx);
    if (!fx_node) return 0.0f;

    /* Find the param track */
    for (int i = 0; i < fx_node->n_params; i++) {
        if (strcmp(fx_node->param_names[i], param_name) == 0) {
            wb_param_track *pt = fx_node->params[i];
            if (pt) {
                return wb_param_track_value_at(pt, time);
            }
        }
    }

    return 0.0f;
}
