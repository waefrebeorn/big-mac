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
typedef struct wb_edit_audio_clip wb_edit_audio_clip;
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

/* ---- edit audio clip --------------------------------------------------- */

struct wb_edit_audio_clip {
    char source_path[512];    /* audio file path (wav/mp3/aac/etc) */
    double start_in_source;   /* seconds into source to start */
    double duration;          /* seconds to play from source */
    double timeline_pos;      /* seconds on timeline where clip starts */
    float volume;             /* volume multiplier (0..1+, 1.0 = unity) */
    float speed;              /* playback speed (1.0 = normal) */
};

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
    /* Audio clips on this track (synchronized with video) */
    wb_edit_audio_clip *audio_clips;
    uint32_t audio_clip_count;
    uint32_t audio_clip_cap;
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

    /* Subtitle overlay (burned in during export) */
    char subtitle_text[256];   /* UTF-8 text (empty = none) */
    float subtitle_pos_x;      /* normalized 0..1 horizontal position */
    float subtitle_pos_y;      /* normalized 0..1 vertical position */
    float subtitle_size;       /* font scale (1.0 = base) */
    uint32_t subtitle_color;   /* RGBA color */

    /* Proxy editing (R085) */
    int proxy_enabled;         /* 1 = use proxies for preview */
    int proxy_w, proxy_h;      /* proxy dimensions */
    char proxy_dir[256];       /* directory for proxy files */
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

/* Auto-cut a clip at scene-change points.
 * Uses wb_video_detect_segments() to find scene boundaries in the source
 * video, then splits the clip at each boundary via wb_edit_split_clip().
 *   threshold: scene change sensitivity (0..1, e.g. 0.3 = sensitive).
 * Returns the number of cuts made (0 if no scenes detected), or -1 on error.
 * The clip must exist and the source_path must be a valid video file. */
int  wb_edit_auto_cut_scenes(wb_edit_graph *g, int track, int clip_idx,
                              float threshold);

/* ---- audio clip management --------------------------------------------- */

/* Add an audio clip to a track. Returns clip index or -1.
 * source: path to audio file (wav/mp3/aac/etc)
 * start: seconds into the source audio to begin playback
 * dur: duration in seconds to play
 * tl_pos: timeline position in seconds where the clip starts */
int  wb_edit_add_audio_clip(wb_edit_graph *g, int track,
                             const char *source,
                             double start, double dur, double tl_pos);

/* Set the volume of an audio clip. vol is a multiplier (0..1+, 1.0 = unity). */
int  wb_edit_set_audio_volume(wb_edit_graph *g, int track, int clip_idx,
                               float vol);

/* ---- audio mixing ------------------------------------------------------- */

/* Mix audio from all tracks into an interleaved float buffer.
 * buf: output buffer (interleaved LRLRLR...), must be n_frames * 2 floats
 * g: edit graph
 * start_time: timeline start time in seconds
 * n_frames: number of frames to mix
 * Returns number of clips that contributed. */
int wb_audio_mix(wb_edit_graph *g, float *buf, double start_time, int n_frames);

/* Get total audio duration in seconds. */
double wb_audio_get_duration(const wb_edit_graph *g);

/* ---- audio muxing ------------------------------------------------------- */

/* Add an AAC audio track to an existing MP4 file from the edit graph's
 * audio clips. Call AFTER wb_edit_render_to_mp4() to add sound.
 * Returns 0 on success, -1 on error. */
int wb_audio_mux_to_mp4(wb_edit_graph *g, const char *mp4_path,
                          volatile int *cancel);

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

/* ---- serialization ----------------------------------------------------- */

/* Save edit graph to a .bedit file. Returns 0 on success, -1 on error. */
int wb_edit_graph_save(const wb_edit_graph *g, const char *path);

/* Load edit graph from a .bedit file. Caller must wb_edit_graph_destroy(). */
wb_edit_graph *wb_edit_graph_load(const char *path);

/* ---- proxy editing ------------------------------------------------------ */

void wb_edit_set_proxy_enabled(wb_edit_graph *g, int enable);
void wb_edit_set_proxy_size(wb_edit_graph *g, int w, int h);
char *wb_edit_generate_proxy(wb_edit_graph *g, const char *source_path);

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

/* ---- subtitle burn-in -------------------------------------------------- */

/* Set subtitle text (empty string or NULL = no subtitle). */
void wb_edit_set_subtitle(wb_edit_graph *g, const char *text);

/* Set subtitle position in normalized 0..1 coords. */
void wb_edit_set_subtitle_position(wb_edit_graph *g, float x, float y);

/* Set subtitle font scale (1.0 = base size). */
void wb_edit_set_subtitle_size(wb_edit_graph *g, float size);

/* Set subtitle color as 0xRRGGBB (alpha defaults to 0xFF). */
void wb_edit_set_subtitle_color(wb_edit_graph *g, uint32_t rgba);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_EDIT_H */
