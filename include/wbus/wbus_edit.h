/* wbus_edit.h — Video edit decision list (EDL) model (R084).
 *
 * A video edit is a node graph evaluated per-frame. This header defines
 * the edit model: tracks, clips, transitions, and the timeline→graph
 * mapping that drives the compositor.
 *
 * Architecture:
 *   - wb_edit_track: a stack of clips with transitions between them
 *   - wb_edit_clip: a region of a source video on the timeline
 *   - wb_edit_transition: a crossfade/effect between two adjacent clips
 *   - wb_edit_graph: the full edit, owns the compositor node graph
 *
 * The edit graph is evaluated at time T by:
 *   1. Finding which clips are active at T (timeline position)
 *   2. Building/pulling the subgraph for those clips
 *   3. Compositing the results
 *
 * C11 only. Opaque structs. No UI/render knowledge.
 */

#ifndef WBUS_EDIT_H
#define WBUS_EDIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "wbus/wbus_compositor.h"

/* Forward declarations */
typedef struct wb_edit_graph wb_edit_graph;
typedef struct wb_edit_track wb_edit_track;
typedef struct wb_edit_clip wb_edit_clip;
typedef struct wb_edit_transition wb_edit_transition;
typedef struct wb_edit_sequence wb_edit_sequence;

/* Transition types (map to compositor transition nodes) */
typedef enum {
    WB_EDIT_TRANS_CROSSFADE = 0,
    WB_EDIT_TRANS_DIP_TO_BLACK,
    WB_EDIT_TRANS_WIPE,
    WB_EDIT_TRANS_DISSOLVE,
    WB_EDIT_TRANS_FLASH,
    WB_EDIT_TRANS_COUNT
} wb_edit_trans_type;

/* ---- edit clip --------------------------------------------------------- */

struct wb_edit_clip {
    char source_path[512];    /* video file path */
    double start_in_source;   /* seconds into source to start */
    double duration;          /* seconds to play from source */
    double timeline_pos;      /* seconds on timeline where clip starts */
    int track;                /* track index */

    /* Per-clip effect chain (node graph rooted here) */
    wb_node *fx_chain;        /* effect chain root (NULL = no FX) */
    wb_node *source_node;     /* video source node for this clip */

    /* Parameters */
    float speed;              /* playback speed (1.0 = normal) */
    float gain;               /* audio gain multiplier */
};

/* ---- edit transition --------------------------------------------------- */

struct wb_edit_transition {
    wb_edit_trans_type type;
    double duration;          /* seconds */
    int clip_a_idx;           /* left clip index on track */
    int clip_b_idx;           /* right clip index on track */
    wb_node *trans_node;      /* compositor transition node */
};

/* ---- edit track -------------------------------------------------------- */

struct wb_edit_track {
    char name[64];
    wb_edit_clip *clips;
    uint32_t clip_count;
    uint32_t clip_cap;
    wb_edit_transition *transitions;
    uint32_t trans_count;
    uint32_t trans_cap;
    int muted;
    int soloed;
    float volume;
};

/* ---- edit graph -------------------------------------------------------- */

struct wb_edit_graph {
    wb_edit_track *tracks;
    uint32_t track_count;
    uint32_t track_cap;

    /* Output compositor nodes */
    wb_node *output_composite; /* final composite of all tracks */
    wb_node *output_node;      /* output node (encoding boundary) */

    /* Color management pipeline (post chain) */
    int color_management_enabled; /* 0 = bypass, 1 = apply post chain */
    wb_cs_mode   input_cs;        /* input colorspace transform */
    wb_cs_mode   output_cs;       /* output colorspace transform */
    wb_tm_op     tonemap;         /* HDR->SDR tonemap operator */
    wb_node     *cs_node;         /* colorspace node (input_cs -> output_cs) */
    wb_node     *tm_node;         /* tonemap node */
    wb_node     *post_output;     /* post chain output (pull endpoint) */

    /* Timeline config */
    double fps;
    int width;
    int height;
    double duration;           /* total timeline duration in seconds */

    /* Evaluation cache */
    double eval_time;          /* last evaluation time */
    wb_frame *eval_frame;      /* cached output frame */
};

/* ---- nested sequence --------------------------------------------------- */
/* A sequence is an edit graph that can be used as a clip inside another
 * sequence. It wraps an inner wb_edit_graph and exposes a source node that,
 * on pull(time), evaluates the inner graph at the given time. This enables
 * compositing a sub-edit (e.g. a pre-edited segment) as a single clip. */

struct wb_edit_sequence {
    wb_edit_graph *graph;      /* inner edit graph (the nested sequence) */
    wb_node       *source_node; /* source node: pull(time) -> eval inner graph */
    double         duration;    /* timeline duration in seconds */
};

/* Create a nested sequence with the given fps and dimensions. */
wb_edit_sequence *wb_edit_sequence_create(double fps, int w, int h);
void              wb_edit_sequence_destroy(wb_edit_sequence *s);

/* Get the inner edit graph for adding tracks/clips. */
wb_edit_graph *wb_edit_sequence_graph(wb_edit_sequence *s);

/* Get the source node that evaluates the inner graph on pull(time).
 * Connect this to a parent sequence's track as a clip source. */
wb_node *wb_edit_sequence_node(wb_edit_sequence *s);

/* ---- lifecycle --------------------------------------------------------- */

wb_edit_graph *wb_edit_graph_create(double fps, int w, int h);
void           wb_edit_graph_destroy(wb_edit_graph *g);

/* ---- track management -------------------------------------------------- */

int  wb_edit_add_track(wb_edit_graph *g, const char *name);
void wb_edit_remove_track(wb_edit_graph *g, int track_idx);

/* ---- clip management --------------------------------------------------- */

/* Add a video clip to a track. Returns clip index or -1. */
int  wb_edit_add_clip(wb_edit_graph *g, int track,
                       const char *source_path,
                       double start_in_source,
                       double duration,
                       double timeline_pos);

/* Remove a clip from its track. */
void wb_edit_remove_clip(wb_edit_graph *g, int track, int clip_idx);

/* Move a clip on the timeline. */
int  wb_edit_move_clip(wb_edit_graph *g, int track, int clip_idx,
                        double new_timeline_pos);

/* Split a clip at a timeline position. Returns new clip index or -1. */
int  wb_edit_split_clip(wb_edit_graph *g, int track, int clip_idx,
                         double split_pos);

/* ---- transitions ------------------------------------------------------- */

/* Add a transition between two adjacent clips. Returns index or -1. */
int  wb_edit_add_transition(wb_edit_graph *g, int track,
                             int clip_a_idx,
                             wb_edit_trans_type type,
                             double duration);

/* ---- effects ----------------------------------------------------------- */

/* Add an effect to a clip's FX chain. The effect is appended. */
int  wb_edit_clip_add_effect(wb_edit_graph *g, int track, int clip_idx,
                              wb_node *effect);

/* ---- evaluation -------------------------------------------------------- */

/* Evaluate the edit graph at time T. Returns a compositor frame.
 * Caller must wb_frame_free() the result. */
wb_frame *wb_edit_graph_evaluate(wb_edit_graph *g, double time_sec);

/* ---- export ------------------------------------------------------------ */

/* Render the entire edit to an MP4 file using libav encoding.
 * Honors cancel flag. Calls prog with progress 0..1. */
int wb_edit_graph_render_to_mp4(wb_edit_graph *g, const char *out_path,
                                 volatile int *cancel,
                                 wb_export_prog_fn prog, void *prog_ctx);

/* Internal: direct render loop (evaluates graph per frame). */
int wb_edit_render_to_mp4(wb_edit_graph *g, const char *out_path,
                           volatile int *cancel,
                           wb_export_prog_fn prog, void *prog_ctx);

/* ---- query ------------------------------------------------------------- */

/* Get the clip active at a timeline position on a track. Returns index or -1. */
int  wb_edit_clip_at(wb_edit_graph *g, int track, double timeline_pos);

/* Get total timeline duration in seconds. */
double wb_edit_graph_get_duration(const wb_edit_graph *g);

/* ---- color management pipeline ----------------------------------------- */

/* Enable (1) or disable (0) the color management post chain. */
void wb_edit_set_color_management(wb_edit_graph *g, int enable);

/* Set the input colorspace transform (applied first in the post chain). */
void wb_edit_set_input_colorspace(wb_edit_graph *g, wb_cs_mode mode);

/* Set the output colorspace transform (applied after input_cs). */
void wb_edit_set_output_colorspace(wb_edit_graph *g, wb_cs_mode mode);

/* Set the HDR->SDR tonemap operator (applied after colorspace transforms). */
void wb_edit_set_tonemap(wb_edit_graph *g, wb_tm_op op);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_EDIT_H */
