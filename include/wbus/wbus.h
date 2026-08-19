#ifndef WUBUS_WBUS_H
#define WUBUS_WBUS_H

/* Big Mac DAW — public API.
 * Sample-accurate, C11, zero-third-party audio workstation.
 * Pull-based engine: the backend requests blocks; the engine renders them.
 * UI and engine talk through a lock-free command queue (see wb_cmd.h).
 */

#include <stdint.h>
#include <stddef.h>
#include "wbus_midifx.h"
#include "wbus_video.h"
#include "wbus_captions.h"

/* Forward declarations for cross-referenced types. */
typedef struct wb_mod_matrix wb_mod_matrix;

#ifdef __cplusplus
extern "C" {
#endif

#define WB_SAMPLE_RATE 44100
#define WB_MAX_CHANNELS 2
#define WB_MAX_BLOCK 4096
#define WB_MAX_TRACKS 128
#define WB_MAX_INSERT_SLOTS 8

typedef float wb_sample;

typedef struct wb_engine wb_engine;

/* ---- transport / timing ---------------------------------------------- */
typedef struct wb_transport {
    int   playing;
    int   recording;
    double song_pos;        /* in samples (double to stay exact over long songs) */
    double bpm;
    int   time_sig_num;     /* e.g. 4 */
    int   time_sig_den;     /* e.g. 4 */
    int   loop_on;
    double loop_start;      /* samples */
    double loop_end;        /* samples */
    double sample_rate;
} wb_transport;

/* ---- events (notes/automation on the timeline) ------------------------ */
typedef struct wb_note {
    double start;    /* sample position */
    double dur;      /* samples */
    uint8_t pitch;   /* MIDI note 0-127 */
    uint8_t vel;     /* 0-127 */
} wb_note;

/* ---- clips ------------------------------------------------------------ */
typedef struct wb_clip {
    int      type;            /* 0 = MIDI/notes, 1 = audio, 2 = video */
    double   start;           /* sample position on timeline (audio) or seconds (video) */
    double   length;          /* samples (audio) or seconds (video) */
    uint32_t note_count;
    wb_note *notes;           /* MIDI clips */
    /* audio clip: owned buffer */
    int      audio_channels;
    uint32_t audio_frames;
    wb_sample *audio_data;
    /* video clip: FFmpeg-backed */
    wb_video_clip *video;      /* non-NULL for type==2 */
    /* R018-C: color-correction "intent" carried into interchange (FCPXML).
     * exposure in stops (0 = none), saturation multiplier (1 = none). */
    float color_exposure;
    float color_saturation;
} wb_clip;

/* ---- mixer insert (one plugin slot on a track) ------------------------ */
typedef struct wb_plugin_slot {
    char   id[64];            /* plugin type id, e.g. "synth", "comp", "reverb" */
    void  *unit;              /* owned by engine */
    int    bypass;            /* per-slot bypass toggle (0 = process, 1 = bypass) */
    float  wet;               /* per-slot wet mix (0.0 = dry, 1.0 = wet) */
} wb_plugin_slot;

/* ---- track kinds ------------------------------------------------------ */
#define WB_TRACK_KIND_INSTR  0   /* MIDI/instrument track */
#define WB_TRACK_KIND_AUDIO  1   /* audio clip track */
#define WB_TRACK_KIND_BUS    2   /* bus/group (audio mix bus) */
#define WB_TRACK_KIND_VIDEO  3   /* video track (FFmpeg-backed, R009) */

/* ---- track ------------------------------------------------------------ */
typedef struct wb_track {
    char       name[64];
    int        kind;          /* WB_TRACK_KIND_* */
    float      volume;        /* linear gain */
    float      pan;           /* -1..1 */
    int        mute;
    int        solo;
    int        route;         /* -1 = master, else index of bus track (kind 2) */
    float      send[WB_MAX_TRACKS];  /* send level to each other track (aux send) */
    int        sidechain[WB_MAX_INSERT_SLOTS];  /* per-slot key source track (-1 = none) */
    uint32_t   clip_count;
    wb_clip   *clips;
    wb_plugin_slot inserts[WB_MAX_INSERT_SLOTS];
} wb_track;

/* ---- automation envelopes ---------------------------------------------- */
typedef struct wb_automation_point {
    double time;    /* song position in samples */
    double value;   /* 0..1 normalized parameter value */
    int    curve;   /* 0=linear, 1=hold, 2=smooth */
} wb_automation_point;

typedef struct wb_automation_lane {
    char    param[64];           /* target parameter, e.g. "volume" */
    int     target;              /* -1 = master, else track index */
    uint32_t point_count;
    wb_automation_point *points;
} wb_automation_lane;

/* ---- session (the editable model) ------------------------------------- */
typedef struct wb_session {
    char      name[128];
    double    bpm;
    int       time_sig_num;
    int       time_sig_den;
    double    length;         /* song length in samples */
    uint32_t  track_count;
    wb_track *tracks;
    uint32_t  automation_count;
    wb_automation_lane **automation;
} wb_session;

/* ---- session lifecycle -------------------------------------------------- */
wb_session *wb_session_create(void);       /* empty session */
wb_session *wb_session_demo(void);         /* 2-track demo song */
void        wb_session_destroy(wb_session *s);
wb_session *wb_session_copy(const wb_session *s); /* deep independent copy */
wb_track   *wb_session_add_track(wb_session *s, const char *name, int kind);
int         wb_session_add_note(wb_track *tr, double start, double dur, int pitch, int vel);
/* Remove the note in `tr` closest to (start,pitch) within a small tolerance.
 * Returns 0 if a note was removed, -1 if none matched. */
int         wb_session_remove_note(wb_track *tr, double start, int pitch);
int         wb_session_add_audio_clip(wb_track *tr, double start, double length,
                                      const wb_sample *data, uint32_t frames,
                                      int channels);

/* ---- undo/redo (session snapshots) -------------------------------------- */
typedef struct wb_undo wb_undo;
wb_undo *wb_undo_create(void);
void     wb_undo_destroy(wb_undo *u);
void     wb_undo_checkpoint(wb_undo *u, const wb_session *current);
int      wb_undo_undo(wb_undo *u, wb_session **owner);
int      wb_undo_redo(wb_undo *u, wb_session **owner);
int      wb_undo_depth(const wb_undo *u);
int  wb_undo_redo_depth(const wb_undo *u);

/* ---- CLAP plugin bridge ------------------------------------------------ */
/* Attach a CLAP host to the engine so that insert ids of the form
 * "clap:<plugin_descriptor_id>" are instantiated and run inside the realtime
 * graph. Call before wb_engine_set_session(). Pass NULL to detach. */
struct wb_clap_host;
void wb_engine_set_clap_host(wb_engine *e, struct wb_clap_host *h);

/* ---- session file save/load (.wbus text format) ----------------------- */
int  wb_session_save(const wb_session *s, const char *path);
wb_session *wb_session_load(const char *path);

/* ---- automation envelopes ---------------------------------------------- */
wb_automation_lane *wb_automation_lane_create(const char *param);
void  wb_automation_lane_destroy(wb_automation_lane *l);
int   wb_automation_add_point(wb_automation_lane *l, double time, double value, int curve);
int   wb_automation_clear(wb_automation_lane *l);
double wb_automation_value_at(const wb_automation_lane *l, double pos, double fallback);

/* ---- automation recording (capture live fader/param moves) ------------- */
typedef struct wb_automation_recorder wb_automation_recorder;
wb_automation_recorder *wb_automation_recorder_create(wb_automation_lane *lane,
                                                      double deadband);
void wb_automation_recorder_destroy(wb_automation_recorder *r);
void wb_automation_recorder_arm(wb_automation_recorder *r, double init_value);
void wb_automation_recorder_disarm(wb_automation_recorder *r);
int  wb_automation_recorder_armed(const wb_automation_recorder *r);
int  wb_automation_recorder_count(const wb_automation_recorder *r);
void wb_automation_recorder_capture(wb_automation_recorder *r, double pos,
                                    double value);
int  wb_automation_recorder_commit(wb_automation_recorder *r);
/* session-level lane ownership */
wb_automation_lane *wb_session_add_automation(wb_session *s, const char *param, int target);
void  wb_session_free_automation(wb_session *s);

/* ---- engine lifecycle ------------------------------------------------- */
wb_engine *wb_engine_create(void);
void       wb_engine_destroy(wb_engine *e);

/* Set the editable session (engine keeps a reference; caller owns memory).
 * Call BEFORE starting playback. Not safe concurrently with render. */
void wb_engine_set_session(wb_engine *e, wb_session *s);
wb_session *wb_engine_get_session(wb_engine *e);

/* ---- transport control (thread-safe via cmd queue) -------------------- */
void wb_engine_play(wb_engine *e);
void wb_engine_stop(wb_engine *e);
void wb_engine_seek(wb_engine *e, double sample_pos);
void wb_engine_set_bpm(wb_engine *e, double bpm);
void wb_engine_get_transport(wb_engine *e, wb_transport *out);

/* ---- parameter/note injection from UI (thread-safe) ------------------- */
void wb_engine_set_track_volume(wb_engine *e, int track, float vol);
void wb_engine_note(wb_engine *e, int track, uint8_t pitch, uint8_t vel);
void wb_engine_set_insert_param(wb_engine *e, int track, int slot, int param, float value);
/* Returns the engine's modulation matrix (may be NULL if engine uninitialized). */
wb_mod_matrix *wb_engine_get_mod_matrix(wb_engine *e);
/* Per-insert slot bypass + wet mix (thread-safe via cmd queue). */
void wb_engine_set_insert_bypass(wb_engine *e, int track, int slot, int on);
void wb_engine_set_insert_wet(wb_engine *e, int track, int slot, float wet);
/* Send/aux routing: set a track's send level to another track (bus or audio).
 * send_level 0.0 = no send; >0 sends a post-FX copy to the destination. */
void wb_engine_set_send_level(wb_engine *e, int src_track, int dst_track, float level);
/* Route a source track's audio into a destination track/slot's key input
 * (compressor sidechain). src_track = -1 clears the sidechain. */
void wb_engine_set_insert_sidechain(wb_engine *e, int track, int slot, int src_track);
/* Insert a MIDI FX unit of the given type into a track's MIDI FX chain slot.
 * Pass WB_MIDIFX_NONE to clear the slot. Returns 0 on success. */
int  wb_engine_set_midifx(wb_engine *e, int track, int slot, wb_midifx_type type);
/* Set a param on a track's MIDI FX unit (see wbus_midifx.h for param meaning). */
void wb_engine_set_midifx_param(wb_engine *e, int track, int slot, int param, float value);

/* ---- MIDI recording into clips ----------------------------------------- */
/* arm/disarm recording on a track's clip. When armed, wb_engine_note events
 * are mirrored into the clip as authored notes (note-ons with the track's
 * current song position; note-offs close matching note-ons for duration). */
void wb_engine_record(wb_engine *e, int track, int clip_idx, int on, int overdub);

/* ---- render ----------------------------------------------------------- */
/* Render up to `n` frames into `out` (interleaved stereo, -1..1).
 * The engine advances transport and produces audio. Returns frames rendered. */
uint32_t wb_engine_render(wb_engine *e, wb_sample *out, uint32_t n);

/* Get instantaneous CPU load estimate (0..1) of the last render. */
float wb_engine_cpu_load(wb_engine *e);

/* Number of Xruns (underruns) since engine start. An Xrun is counted when
 * the render callback could not take the process lock (a non-RT thread was
 * mid-edit) and had to drop a block rather than block the audio thread. */
uint64_t wb_engine_xruns(wb_engine *e);

/* Begin/end a non-RT edit (session structure change). render() try-locks
 * this; if it's held at block time, render counts an Xrun and returns
 * silence rather than blocking the audio thread. */
void wb_engine_begin_edit(wb_engine *e);
void wb_engine_end_edit(wb_engine *e);

/* Convenience: render the whole session to an interleaved buffer (caller frees). */
int wb_engine_render_session(wb_engine *e, wb_session *s, wb_sample **out, uint32_t *frames);

/* ---- video editor API (R009/R011) ------------------------------------- */

/* Add a video track to the session. Returns track index or -1 on error. */
int  wb_session_add_video_track(wb_session *s, const char *name);

/* Add a video clip on a video track. The clip references an FFmpeg-decodable
 * source file. Proxy is generated automatically at import. Returns clip index
 * or -1 on error. */
int  wb_session_add_video_clip(wb_session *s, int track, const char *source_path,
                               double timeline_pos);

/* R018-C: set a clip's color-correction "intent" (carried into FCPXML).
 * exposure in stops (0 = none), saturation multiplier (1 = none). */
void wb_clip_set_color(wb_clip *cl, float exposure, float saturation);

/* Set a proxy path on an existing video clip (UI import, post-proxy-gen). */
int  wb_session_set_video_proxy(wb_session *s, int track, int clip,
                                const char *proxy_path);

/* Get the video clip on a track at a given timeline position (seconds).
 * Returns clip index or -1 if no clip at that position. */
int  wb_session_video_clip_at(wb_session *s, int track, double timeline_pos);

/* Export codec selection (R018-A): H.264 for delivery, ProRes for
 * professional editorial interchange (the standard NLE exchange format). */
typedef enum {
    WB_VIDEO_CODEC_H264 = 0,   /* libx264 yuv420p mp4 — delivery */
    WB_VIDEO_CODEC_PRORES,     /* prores_ks yuv422p10le mov — editorial */
    WB_VIDEO_CODEC_PRORES_HQ   /* prores_ks profile 3 (HQ) */
} wb_video_codec;

/* Export the session as a 1080p60 file with optional caption burn.
 * - codec: select H.264 (mp4) or ProRes (mov) output container
 * - srt_path: optional SRT to burn as subtitles (NULL = no captions)
 * - output_path: final file (.mp4 for H264, .mov for ProRes)
 * Returns 0 on success. */
int  wb_video_export_codec(wb_session *s, wb_engine *e,
                           const char *output_path,
                           const char *srt_path,
                           wb_video_codec codec);

/* Legacy H.264 wrapper (delegates to wb_video_export_codec). */
int  wb_video_export(wb_session *s, wb_engine *e,
                     const char *output_path,
                     const char *srt_path);

/* Quick captions-only step: extract audio, transcribe, produce SRT.
 * Used during export or as a standalone feature. */
int  wb_video_generate_captions(wb_session *s, int video_track,
                                const char *srt_out_path);

/* Flat wrappers (no context needed) — for wb_daw.c video tab shortcuts */
int  wb_video_captions_generate(const char *video_path, const char *srt_out_path,
                                const char *cli_path, const char *model_path);
int  wb_video_captions_burn(const char *input_path, const char *output_path,
                            const char *srt_path, const char *ffmpeg_path);

/* Delete a video clip from a track. Returns 0 on success. */
int  wb_session_remove_video_clip(wb_session *s, int track, int clip);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_H */
