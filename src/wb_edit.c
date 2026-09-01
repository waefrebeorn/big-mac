/* wb_edit.c — Video edit decision list (EDL) model (R084).
 *
 * Implements the edit graph: tracks, clips, transitions, and the
 * timeline→compositor mapping. Evaluation at time T finds active clips,
 * pulls their source+FX subgraphs, and composites the results.
 */

#include "wbus/wbus_edit.h"
#include "wbus/wbus_video.h"
#include "wbus/wbus_compositor.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
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
