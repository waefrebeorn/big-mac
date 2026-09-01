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

    /* Timeline config */
    double fps;
    int width;
    int height;
    double duration;           /* total timeline duration in seconds */

    /* Evaluation cache */
    double eval_time;          /* last evaluation time */
    wb_frame *eval_frame;      /* cached output frame */
};

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

/* ---- query ------------------------------------------------------------- */

/* Get the clip active at a timeline position on a track. Returns index or -1. */
int  wb_edit_clip_at(wb_edit_graph *g, int track, double timeline_pos);

/* Get total timeline duration in seconds. */
double wb_edit_graph_get_duration(const wb_edit_graph *g);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_EDIT_H */
