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

/* ---- audio effects ----------------------------------------------------- */

/* Audio effect types (per-clip FX chain) */
typedef enum {
    WB_AUDIO_FX_NONE = 0,
    WB_AUDIO_FX_EQ,         /* 4-band parametric EQ */
    WB_AUDIO_FX_REVERB,     /* Schroeder FDN reverb */
    WB_AUDIO_FX_COMPRESSOR, /* VCA compressor/limiter */
    WB_AUDIO_FX_DELAY,      /* stereo delay/echo */
    WB_AUDIO_FX_DISTORTION, /* waveshaping distortion */
    WB_AUDIO_FX_CHORUS,     /* chorus/flanger */
    WB_AUDIO_FX_VST3,       /* VST3 plugin instance */
    WB_AUDIO_FX_COUNT
} wb_audio_fx_type;

/* Audio effect parameters (union-like, type selects valid fields) */
typedef struct {
    wb_audio_fx_type type;
    int enabled;              /* 0 = bypass, 1 = active */
    float mix;                /* wet/dry 0..1 */

    /* EQ params */
    struct {
        float low_gain;       /* dB, -18..+18 */
        float mid_gain;       /* dB, -18..+18 */
        float high_gain;      /* dB, -18..+18 */
        float presence_gain;  /* dB, -18..+18 */
    } eq;

    /* Reverb params */
    struct {
        float room_size;      /* 0..1 */
        float damping;        /* 0..1 */
        float wet;            /* 0..1 */
    } reverb;

    /* Compressor params */
    struct {
        float threshold_db;   /* e.g. -12.0 */
        float ratio;          /* e.g. 4.0 */
        float attack_ms;      /* e.g. 5.0 */
        float release_ms;     /* e.g. 120.0 */
        float makeup_db;      /* e.g. 3.0 */
    } compressor;

    /* Delay params */
    struct {
        float time_ms;        /* delay time in ms */
        float feedback;       /* 0..1 */
        float wet;            /* 0..1 */
    } delay;

    /* Distortion params */
    struct {
        float drive;          /* 0..1 */
        float tone;           /* 0..1 */
        float level;          /* output level 0..1 */
    } distortion;

    /* Chorus params */
    struct {
        float rate_hz;        /* LFO speed */
        float depth_ms;       /* modulation depth in ms */
        float mix;            /* 0..1 */
    } chorus;

    /* VST3 params */
    struct {
        char plugin_name[256]; /* plugin name from wb_vst3_scan */
        void *instance;        /* opaque VST3 handle */
        int param_count;       /* number of automatable parameters */
    } vst3;
} wb_audio_fx;

#define WB_AUDIO_FX_PER_CLIP 8

/* ---- edit audio clip --------------------------------------------------- */

struct wb_edit_audio_clip {
    char source_path[512];    /* audio file path (wav/mp3/aac/etc) */
    double start_in_source;   /* seconds into source to start */
    double duration;          /* seconds to play from source */
    double timeline_pos;      /* seconds on timeline where clip starts */
    float volume;             /* volume multiplier (0..1+, 1.0 = unity) */
    float speed;              /* playback speed (1.0 = normal) */
    float pan;                /* pan position: -1 (left) .. +1 (right), 0 = center */

    /* Per-clip audio effects chain */
    wb_audio_fx fx_chain[WB_AUDIO_FX_PER_CLIP];
    int fx_count;
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

/* Set the pan of an audio clip. pan ranges from -1 (full left) to +1 (full right),
 * 0 = center. Returns 0 on success, -1 on error. */
int  wb_edit_set_audio_pan(wb_edit_graph *g, int track, int clip_idx,
                            float pan);

/* Set an audio effect on a clip's FX chain.
 * track: track index, clip_idx: audio clip index
 * fx_slot: slot index (0..WB_AUDIO_FX_PER_CLIP-1)
 * fx: the effect to set (type + params)
 * Returns 0 on success, -1 on error. */
int  wb_edit_set_audio_fx(wb_edit_graph *g, int track, int clip_idx,
                           int fx_slot, const wb_audio_fx *fx);

/* Clear an audio effect slot. Returns 0 on success, -1 on error. */
int  wb_edit_clear_audio_fx(wb_edit_graph *g, int track, int clip_idx,
                             int fx_slot);

/* ---- audio mixing ------------------------------------------------------- */

/* Mix audio from all tracks into an interleaved float buffer.
 * buf: output buffer (interleaved LRLRLR...), must be n_frames * 2 floats
 * g: edit graph
 * start_time: timeline start time in seconds
 * n_frames: number of frames to mix
 * Returns number of clips that contributed. */
int wb_audio_mix(wb_edit_graph *g, float *buf, double start_time, int n_frames);

/* Mix audio from all tracks into 5.1 surround (6 channels).
 * Outputs planar (non-interleaved) audio: each channel buffer must be
 * n_frames floats. Channel order: L, R, C, LFE, Ls, Rs.
 * Pan distribution:
 *   - Center pan (pan == 0) -> C channel only
 *   - Left pan (pan < 0)    -> L + Ls (gain scales with |pan|)
 *   - Right pan (pan > 0)   -> R + Rs (gain scales with |pan|)
 *   - LFE gets a low-passed version of the full mix (sum of all channels)
 * g: edit graph
 * start_time: timeline start time in seconds
 * n_frames: number of frames to mix
 * Returns number of clips that contributed. */
int wb_audio_mix_surround(wb_edit_graph *g, float *ch_L, float *ch_R,
                          float *ch_C, float *ch_LFE, float *ch_Ls, float *ch_Rs,
                          double start_time, int n_frames);

/* Get total audio duration in seconds. */
double wb_audio_get_duration(const wb_edit_graph *g);

/* ---- audio muxing ------------------------------------------------------- */

/* Add an AAC audio track to an existing MP4 file from the edit graph's
 * audio clips. Call AFTER wb_edit_render_to_mp4() to add sound.
 * Returns 0 on success, -1 on error. */
int wb_audio_mux_to_mp4(wb_edit_graph *g, const char *mp4_path,
                          volatile int *cancel);

/* ---- audio mixer ---------------------------------------------------------- */

void wb_audio_mixer_init(void);
void wb_audio_mixer_sync(wb_edit_graph *g);
void wb_audio_mixer_set_volume(int track, float vol);
void wb_audio_mixer_set_pan(int track, float pan);
void wb_audio_mixer_set_mute(int track, int mute);
void wb_audio_mixer_set_solo(int track, int solo);
void wb_audio_mixer_set_master_volume(float vol);
float wb_audio_mixer_get_vu(int track);
int wb_audio_mixer_get_track_count(void);

/* Set the output channel configuration.
 * channels: 2 = stereo, 6 = 5.1 surround. Returns 0 on success, -1 on error. */
int wb_audio_mixer_set_channels(int channels);

/* Query whether 5.1 surround output is active (1) or stereo (0). */
int wb_audio_mixer_is_surround(void);

/* ---- keyframe animation ------------------------------------------------ */

/* Add a keyframe to an FX node's parameter.
 * track: track index, clip: clip index, fx: index in FX chain (0=first)
 * param_name: name of the parameter (e.g. "intensity", "levels")
 * time: time in seconds within the clip
 * value: parameter value at this keyframe
 * Returns 0 on success, -1 on error. */
int wb_edit_set_keyframe(wb_edit_graph *g, int track, int clip, int fx,
                          const char *param_name, double time, float value);

/* Get the animated value of an FX param at a given time.
 * Returns the value, or fallback if no keyframes are set. */
float wb_edit_get_keyframed_value(wb_edit_graph *g, int track, int clip,
                                   int fx, const char *param_name, double time);

/* ---- undo/redo ---------------------------------------------------------- */

void wb_edit_undo_init(void);
void wb_edit_undo_shutdown(void);
void wb_edit_undo_checkpoint(void);
void wb_edit_undo_set_current(wb_edit_graph *g);
wb_edit_graph *wb_edit_undo_undo(wb_edit_graph *current);
wb_edit_graph *wb_edit_undo_redo(wb_edit_graph *current);
int wb_edit_undo_can_undo(void);
int wb_edit_undo_can_redo(void);

/* ---- multi-camera editing ----------------------------------------------- */

/* Create a multi-camera group from clips on different tracks.
 * track_indices[i], clip_indices[i] identify each angle.
 * All clips should be at the same timeline position.
 * Returns group index or -1. */
int wb_multicam_create_group(wb_edit_graph *g, const char *name,
                              int *track_indices, int *clip_indices,
                              int num_angles, double timeline_pos);

/* Switch active angle. Returns 0 on success, -1 on error. */
int wb_multicam_set_active_angle(int group_idx, int angle);

/* Get active angle's source path. */
const char *wb_multicam_get_active_path(int group_idx);

/* Expand group to individual tracks. */
int wb_multicam_expand(int group_idx);

/* Get number of multicam groups. */
int wb_multicam_count(void);

/* Clear all multicam groups. */
void wb_multicam_clear(void);

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

/* ---- text-based editing ----------------------------------------------- */

/* Parse a transcript JSON and generate edit cuts.
 * transcript_json: JSON array of [{"start":0.0,"end":1.5,"text":"hello"}, ...]
 * edit_rules: comma-separated rules (e.g. "remove=um,uh,like" or "keep-all")
 * For each transcript entry, finds overlapping clips on all tracks and splits
 * them at word boundaries. Applies edit_rules to mark segments for removal.
 * Returns the number of edit operations performed, or -1 on error. */
int wb_edit_transcript_to_edits(wb_edit_graph *g, const char *transcript_json,
                                 const char *edit_rules);

/* Search transcript entries for clips containing specific words.
 * query: space-separated words to search for (case-insensitive).
 * Prints matching clip info to stdout. Returns number of matches, or -1 on error. */
int wb_edit_search_transcript(wb_edit_graph *g, const char *query);

/* Auto-detect and remove silent segments from audio clips.
 * threshold_db: RMS threshold in dB (e.g. -40.0). Segments below this are silent.
 * min_duration: minimum duration in seconds for a segment to be considered silence.
 * Analyzes audio buffer RMS for each audio clip, marks silent segments for removal
 * by splitting and removing them from the timeline.
 * Returns the number of silent segments removed, or -1 on error. */
int wb_edit_delete_silence(wb_edit_graph *g, float threshold_db,
                            float min_duration);

/* Undo the last text-based edit operation (transcript cuts, silence removal).
 * Uses the same undo stack as wb_edit_undo_undo() but only pops text-edit ops.
 * Returns 0 on success, -1 if nothing to undo or error. */
int wb_edit_text_undo(wb_edit_graph *g);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_EDIT_H */
