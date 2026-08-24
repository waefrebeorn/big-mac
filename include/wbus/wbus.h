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
#include "wbus_clip_edit.h"
#include "wbus_transcript.h"
#include "wbus_import.h"

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
    int   snap;             /* G10: quantize edits to grid */
} wb_transport;

/* ---- events (notes/automation on the timeline) ------------------------ */
typedef struct wb_note {
    double start;    /* sample position */
    double dur;      /* samples */
    uint8_t pitch;   /* MIDI note 0-127 */
    uint8_t vel;     /* 0-127 */
    /* G86: multi-CC lanes. Per-note controller snapshots shown/edited in the
     * lane editors. Defaults 0 (mod off) / 0 (no aftertouch). */
    uint8_t mod;     /* CC1 modulation wheel 0-127 */
    uint8_t atouch;  /* channel aftertouch 0-127 */
} wb_note;

/* ---- clips ------------------------------------------------------------ */
typedef struct wb_clip {
    int      type;            /* 0 = MIDI/notes, 1 = audio, 2 = video, 3 = performance */
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
    /* R068: performance clip — self-contained event snapshot. The
     * wb_perfclip is created from a live perf and owns its event list. */
    void *perfclip;            /* non-NULL for type==3 (wb_perfclip*) */
    /* R018-C: color-correction "intent" carried into interchange (FCPXML).
     * exposure in stops (0 = none), saturation multiplier (1 = none). */
    float color_exposure;
    float color_saturation;
    /* R022: clip gain (region gain). Linear multiplier applied to the audio
     * clip BEFORE any track processing/fader — i.e. pre-fader gain staging,
     * exactly like Pro Tools "Region Gain" / Reaper "Item Gain". 1.0 = unity. */
    float clip_gain;
    /* R030: take-lane. Clips with lane != track.active_lane are NOT played
     * (only the active lane is heard — Pro Tools "main playlist" comping).
     * 0 = main lane. */
    int lane;
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
    /* G30: two named aux sends (SEND A / SEND B) targeting bus tracks. */
    float      send_level[2]; /* 0..1 send amount */
    int        send_target[2];/* destination bus track index, -1 = none */
    /* G74: per-send pre/post-fader switch (0 = post-fader default, 1 = pre). */
    int        send_pre[2];
    int        sidechain[WB_MAX_INSERT_SLOTS];  /* per-slot key source track (-1 = none) */
    uint32_t   clip_count;
    wb_clip   *clips;
    wb_plugin_slot inserts[WB_MAX_INSERT_SLOTS];
    /* R024: live meter — actual signal level after processing (not fader).
     * peak = max abs sample this block; rms = sqrt(mean square). Both in
     * linear 0..~1+. Ballistics (decay) applied in the UI for VU feel. */
    float meter_peak;
    float meter_rms;
    /* R030: take-lanes — only the clip on this lane is heard; switch to
     * audition/comp another take. 0 = main lane. */
    int active_lane;
    /* G09 (Wave2): per-track record-arm (arrangement REC button). */
    int rec_armed;
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
    /* G25: automation modes. 0=READ (lane drives param), 1=WRITE (fader owns
     * param while armed), 2=TOUCH (write only while touched), 3=LATCH (write
     * from first touch until stopped). While writing, stage_automation does
     * not override the live fader value. */
    int     mode;
    int     writing;             /* set by UI during an active write pass */
} wb_automation_lane;

/* ---- arrangement markers (song sections / cues) ----------------------- */
/* A marker labels a point on the timeline. 'section' markers (Intro/Verse/
 * Chorus...) describe song structure; 'cue' markers are plain locators.
 * Modeled on Ardour location markers / Logic arrangement markers. */
typedef struct wb_marker {
    double pos;        /* sample position on the timeline */
    char   label[32];  /* e.g. "Intro", "Verse", "Chorus", "Hook" */
    int    kind;       /* 0 = cue (locator), 1 = section (song part) */
} wb_marker;

/* ---- media bin entry (G04) ----------------------------------------------- */
/* One row in the app-level media bin: a source asset that has been imported
 * (or scanned in the browser). `offline` is 1 when the source file no longer
 * exists on disk (G70 relink/offline handling). */
typedef struct wb_bin_entry {
    char     path[1024];     /* source file path (absolute or relative) */
    int      kind;           /* 0 = audio, 1 = video */
    double   duration;       /* seconds */
    char     name[256];      /* display name (basename of the file) */
    int      offline;        /* 1 if the source file is missing on disk */
    int      color;          /* G68: label slot 0..7 (0 = none) for bin views */
} wb_bin_entry;

/* G82: one chord event on the chord track. */
typedef struct wb_chord_ev {
    double pos;        /* sample position */
    int    root;       /* 0..11 */
    int    type;       /* 0..4, same table as the scale tools */
} wb_chord_ev;

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
    /* R022: arrangement markers */
    uint32_t  marker_count;
    wb_marker markers[64];
    /* G82: chord track — harmonic grid. Each chord holds from its `pos`
     * until the next chord. root 0..11, type 0..4 (same scale table). */
    uint32_t   chord_count;
    wb_chord_ev chords[64];
    /* G89: swing amount, fraction of a 16th note delayed on odd steps
     * (Roger Linn MPC spec: 50% = straight .. 75%; stored 0..0.6 as the
     * DELAY FRACTION, i.e. 0.25 == MPC 75%). 0 = straight. */
    double    swing;
    /* G04/G70 (Wave 3 lane C): app-level media bin — the persistent list of
     * imported assets. Each entry records the source path, kind, duration and
     * display name; `offline` is set on load when the source is missing. */
    uint32_t  bin_count;
#define WB_MAX_BIN 256
    wb_bin_entry bin_entries[WB_MAX_BIN];
} wb_session;

/* G69: multiple timelines (sequences) per project. A wb_project owns N
 * sessions; exactly one is "active" (the one the engine renders). Session
 * 0 is always present so single-timeline users see no difference. The
 * project file format appends "sequence" sections after the base grammar;
 * a file without them loads as a one-sequence project (backward compat). */
#define WB_MAX_SEQUENCES 16
typedef struct wb_project wb_project;

/* ---- project (multi-sequence container) --------------------------------- */
wb_project *wb_project_create(void);              /* one empty sequence */
wb_project *wb_project_from_session(wb_session *owned); /* adopt a session */
void         wb_project_destroy(wb_project *p);
int          wb_project_sequence_count(const wb_project *p);
wb_session  *wb_project_sequence(const wb_project *p, int i);
wb_session  *wb_project_active(const wb_project *p);
int          wb_project_active_index(const wb_project *p);
int          wb_project_add_sequence(wb_project *p, const char *name); /* ret idx */
int          wb_project_remove_sequence(wb_project *p, int i);   /* never 0 */
int          wb_project_set_active(wb_project *p, int i);
int          wb_project_save(const wb_project *p, const char *path);
wb_project  *wb_project_load(const char *path);

/* ---- session lifecycle -------------------------------------------------- */
wb_session *wb_session_create(void);       /* empty session */
wb_session *wb_session_demo(void);         /* 2-track demo song */
void        wb_session_destroy(wb_session *s);
wb_session *wb_session_copy(const wb_session *s); /* deep independent copy */
wb_track   *wb_session_add_track(wb_session *s, const char *name, int kind);
int          wb_session_remove_track(wb_session *s, uint32_t idx); /* G09 */
int          wb_session_move_track(wb_session *s, uint32_t idx, int delta); /* G09 */
int         wb_session_add_note(wb_track *tr, double start, double dur, int pitch, int vel);
/* Remove the note in `tr` closest to (start,pitch) within a small tolerance.
 * Returns 0 if a note was removed, -1 if none matched. */
int         wb_session_remove_note(wb_track *tr, double start, int pitch);
int         wb_session_add_audio_clip(wb_track *tr, double start, double length,
                                      const wb_sample *data, uint32_t frames,
                                      int channels);
/* R022: arrangement markers (song-structure labels on the timeline) */
int         wb_session_add_marker(wb_session *s, double pos, const char *label, int kind);
/* R030: take-lanes — set which lane is heard on a track (comping). Only clips
 * on the active lane are played; others are muted. 0 = main lane. */
void        wb_session_set_active_lane(wb_session *s, int track, int lane);
/* R031: comping — promote the time-region [t0,t1] of a take-lane audio clip
 * onto lane 0 (the comp). Returns # comp clips made, or -1 on error. */
int         wb_session_comp_region(wb_session *s, int track, int src_lane, double t0, double t1);
/* G14: direct-manipulation clip move/trim (mouse drag model ops).
 * move_clip relocates clip `clip` of `track` to (new_track, new_start).
 * Only allowed between tracks hosting the same clip type; new_start is
 * clamped to >= 0. Returns 0 on success, -1 on bad indices/type mismatch.
 * NOTE: callers that keep a wb_clip_edit_table must migrate the entry with
 * wb_clip_edit_move() (the table keys are (track,clip) indices). */
int         wb_session_move_clip(wb_session *s, int track, int clip,
                                 int new_track, double new_start);
/* G14: trim the head/tail by `delta` samples (positive delta = shorten head /
 * extend tail). Audio clips keep buffer alignment via start_in_source in
 * `ed` (may be NULL to skip side-table bookkeeping). MIDI clips shift start
 * and clamp notes into [0,length]. Returns 0 on success, -1 on error. */
int         wb_session_trim_clip_head(wb_session *s, void *ed,
                                      int track, int clip, double delta);
int         wb_session_trim_clip_tail(wb_session *s, void *ed,
                                      int track, int clip, double delta);

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

/* R043 (G1/G2): access the engine's clip-edit side-table (fade/offset handles).
 * The table is engine-owned; callers get/set per-clip edit state through it.
 * Returns NULL if the engine or table is unavailable. */
wb_clip_edit_table *wb_engine_clip_edit(wb_engine *e);

/* ---- transport control (thread-safe via cmd queue) -------------------- */
void wb_engine_play(wb_engine *e);
void wb_engine_stop(wb_engine *e);
void wb_engine_seek(wb_engine *e, double sample_pos);
void wb_engine_set_bpm(wb_engine *e, double bpm);
void wb_engine_get_transport(wb_engine *e, wb_transport *out);
void wb_engine_set_loop(wb_engine *e, double start, double end);   /* G10 */
void wb_engine_set_snap(wb_engine *e, int on);                    /* G10 */

/* ---- parameter/note injection from UI (thread-safe) ------------------- */
void wb_engine_set_track_volume(wb_engine *e, int track, float vol);
/* G35: set the master bus volume directly (0..1+). */
void wb_engine_set_master_volume(wb_engine *e, float vol);
void wb_engine_note(wb_engine *e, int track, uint8_t pitch, uint8_t vel);
/* R037: SESSION-view clip launching (transport-independent loop playback).
 * wb_engine_launch toggles a clip playing from its start; passing the already-
 * launched clip index stops it. wb_engine_launched_clip returns the index or
 * -1. */
void wb_engine_launch(wb_engine *e, int track, int clip_idx);
void wb_engine_stop_launch(wb_engine *e, int track);
int  wb_engine_launched_clip(wb_engine *e, int track);
void wb_engine_set_insert_param(wb_engine *e, int track, int slot, int param, float value);
/* Returns the engine's modulation matrix (may be NULL if engine uninitialized). */
wb_mod_matrix *wb_engine_get_mod_matrix(wb_engine *e);
/* R028: master bus output meters (post master-volume, pre-dac). peak/rms are
 * linear 0..N amplitudes; pass NULL for either to ignore. */
void wb_engine_get_master_meter(wb_engine *e, float *peak, float *rms);
/* G32: live K-weighted readings from the master path. lufs_st = short-term
 * LUFS (smoothed, 0.0 = silence/unset), true_peak = linear true peak
 * (0..1+, slow-release hold). Either pointer may be NULL. */
void wb_engine_get_master_lufs(wb_engine *e, float *lufs_st, float *true_peak);
/* Per-insert slot bypass + wet mix (thread-safe via cmd queue). */
void wb_engine_set_insert_bypass(wb_engine *e, int track, int slot, int on);
void wb_engine_set_insert_wet(wb_engine *e, int track, int slot, float wet);
/* Send/aux routing: set a track's send level to another track (bus or audio).
 * send_level 0.0 = no send; >0 sends a post-FX copy to the destination. */
void wb_engine_set_send_level(wb_engine *e, int src_track, int dst_track, float level);
/* G30/G74: configure one of a track's two named aux sends (slot 0 = SEND A,
 * 1 = SEND B). target = destination bus track index or -1; level 0..1;
 * pre != 0 taps BEFORE the fader, pre == 0 (default) AFTER fader gain. */
void wb_engine_set_send(wb_engine *e, int src_track, int slot, int target,
                        float level, int pre);
/* G89: swing. Returns the delay in samples applied at timeline position
 * 'pos' for the given bpm and swing fraction (0..0.6). Odd 16th-note steps
 * are delayed by swing*sixteenth; even steps return 0 (Roger Linn spec). */
double wb_swing_offset(double bpm, double swing, double pos);
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
/* G34: number of VST3 plugins quarantined for emitting bad output. */
int wb_engine_vst3_faults(wb_engine *e);

/* Begin/end a non-RT edit (session structure change). render() try-locks
 * this; if it's held at block time, render counts an Xrun and returns
 * silence rather than blocking the audio thread. */
void wb_engine_begin_edit(wb_engine *e);
void wb_engine_end_edit(wb_engine *e);

/* Convenience: render the whole session to an interleaved buffer (caller frees). */
int wb_engine_render_session(wb_engine *e, wb_session *s, wb_sample **out, uint32_t *frames);
/* G33: bounce a single track offline — temporarily mutes every other track
 * (and the target's mute state is ignored), renders the session length,
 * then restores the previous mute states. Returns 0; caller frees *out. */
int wb_engine_render_track(wb_engine *e, wb_session *s, int track,
                           wb_sample **out, uint32_t *frames);
/* G71: render cache — pre-render the session to a preview WAV for smooth
 * scrubbing. Invalidated automatically when length/bpm change. */
int wb_engine_build_render_cache(wb_session *s,
                                 void (*cb)(void*,double), void *cbctx,
                                 int *cancelled);
int wb_engine_invalidate_render_cache(void);

/* G38: progress/cancel-aware variant. cb (optional) is invoked between render
 * chunks with progress mapped onto [lo,hi]. Returns 1 when cancelled, -1 on
 * error, 0 on success (on cancel the caller owns freeing nothing — *out=NULL). */
int wb_engine_render_session_prog(wb_engine *e, wb_session *s, wb_sample **out,
                                  uint32_t *frames,
                                  void (*cb)(void *, double), void *cbctx,
                                  double lo, double hi,
                                  volatile int *cancel);

/* ---- video editor API (R009/R011) ------------------------------------- */

/* Add a video track to the session. Returns track index or -1 on error. */
int  wb_session_add_video_track(wb_session *s, const char *name);

/* Add a video clip on a video track. The clip references an FFmpeg-decodable
 * source file. Proxy is generated automatically at import. Returns clip index
 * or -1 on error. */
int  wb_session_add_video_clip(wb_session *s, int track, const char *source_path,
                               double timeline_pos);

/* R068: add a performance clip (a snapshot of a recorded wb_perf) onto a
 * video track. The clip owns a copy of the perf's event log so it is
 * reproducible without the live perf. Returns clip index or -1. */
int  wb_session_add_perf_clip(wb_session *s, int track, void *perfclip,
                              double timeline_pos, double duration);

/* R018-C: set a clip's color-correction "intent" (carried into FCPXML).
 * exposure in stops (0 = none), saturation multiplier (1 = none). */
void wb_clip_set_color(wb_clip *cl, float exposure, float saturation);

/* Set a proxy path on an existing video clip (UI import, post-proxy-gen). */
int  wb_session_set_video_proxy(wb_session *s, int track, int clip,
                                const char *proxy_path);

/* Get the video clip on a track at a given timeline position (seconds).
 * Returns clip index or -1 if no clip at that position. */
int  wb_session_video_clip_at(wb_session *s, int track, double timeline_pos);

/* ---- G04/G70 (Wave 3 lane C): media bin + relink/offline ----------------- */
/* Append an asset to the session's media bin (called by every import path:
 * browser click, audio/video import). The entry is marked offline if the file
 * does not yet exist. Returns the new bin index, or -1 if the bin is full. */
int  wb_session_add_bin_entry(wb_session *s, const char *path, int kind,
                             double duration);
/* G68: sort the media bin (0 = name, 1 = kind+name, 2 = duration). */
void wb_session_sort_bin(wb_session *s, int mode);
/* G76: FX chain rack — export/import a track's insert chain as text
 * ("eq|chorus|-|delay"; "-" clears a slot). */
int  wb_session_export_chain(const wb_session *s, int track,
                             char *out, int cap);
int  wb_session_import_chain(wb_session *s, int track, const char *chain);
/* G28: strip silence — split an audio clip into its loud regions
 * (thresh linear 0..1, min_sec minimum region length). Returns region count;
 * 0 = all silent (clip removed), -1 error. */
int  wb_session_strip_silence(wb_session *s, int track, int clip,
                              float thresh, double min_sec);
/* G82: chord track — add/clear chords; resolve the chord at a position.
 * Returns the new chord index / count / -1. */
int         wb_session_add_chord(wb_session *s, double pos, int root, int type);
void        wb_session_clear_chords(wb_session *s);
int         wb_session_chord_at(const wb_session *s, double pos,
                                int *root, int *type);
/* G82: snap a MIDI clip's notes into whichever chord sounds at each note. */
int  wb_session_snap_to_chords(wb_session *s, int track, int clip);
/* G22: swap two clips' timeline positions (alt+drop). Same media kind only. */
int  wb_session_swap_clips(wb_session *s, int track_a, int clip_a,
                           int track_b, int clip_b);
/* G83: MIDI transformations — 0 humanize, 1 randomize-velocities,
 * 2 arpeggiate-up, 3 strum. Returns notes touched or -1. */
int  wb_session_transform_notes(wb_session *s, int track, int clip, int mode);
/* G84: articulation management — named articulations hide raw keyswitches. */
int         wb_session_set_articulation(wb_session *s, int track, int art_id);
const char *wb_articulation_name(int art_id);
int         wb_articulation_keyswitch(int art_id);
int         wb_articulation_count(void);

/* (Re)compute the `offline` flag of every bin entry and video clip from disk
 * existence. Called on project load (G70) and before relink searches. */
void wb_session_update_offline(wb_session *s);

/* G70: scan each offline bin entry's original directory plus ~/Movies,
 * ~/Desktop and ~/Documents for a file with the same basename; when found,
 * update the bin entry's path (and mirror it onto any matching video clip's
 * source_path) and clear the offline flag. Returns the number relinked. */
int  wb_session_relink_bin(wb_session *s);

/* Export codec selection (R018-A): H.264 for delivery, ProRes for
 * professional editorial interchange (the standard NLE exchange format). */
typedef enum {
    WB_VIDEO_CODEC_H264 = 0,   /* libx264 yuv420p mp4 — delivery */
    WB_VIDEO_CODEC_PRORES,     /* prores_ks yuv422p10le mov — editorial */
    WB_VIDEO_CODEC_PRORES_HQ   /* prores_ks profile 3 (HQ) */
} wb_video_codec;

/* G38/G39/G40 (R072): full export with RANGE, RESOLUTION and PROGRESS/CANCEL.
 * - range_start/range_dur: seconds; range_start < 0 exports the WHOLE session.
 *   Applied as accurate output-side -ss/-t so video AND audio stay in sync.
 * - res_h: output height (0 or 1080 = no scaling; 480/720 scales preserving AR).
 * - prog: optional callback invoked between chunks with progress in 0..1.
 * - cancel: optional flag polled between chunks; set it non-zero to abort
 *   (the ffmpeg child, if running, is killed with SIGTERM). Returns -2 when
 *   cancelled, -1 on error, 0 on success. */
typedef void (*wb_export_prog_fn)(void *ctx, double progress);
int  wb_video_export_full(wb_session *s, wb_engine *e,
                          const char *output_path,
                          const char *srt_path,
                          wb_video_codec codec,
                          double range_start, double range_dur,
                          int res_h,
                          wb_export_prog_fn prog, void *prog_ctx,
                          volatile int *cancel);

/* Export the session as a 1080p60 file with optional caption burn.
 * - codec: select H.264 (mp4) or ProRes (mov) output container
 * - srt_path: optional SRT to burn as subtitles (NULL = no captions)
 * - output_path: final file (.mp4 for H264, .mov for ProRes)
 * Returns 0 on success. */
int  wb_video_export_codec(wb_session *s, wb_engine *e,
                           const char *output_path,
                           const char *srt_path,
                           wb_video_codec codec);

/* Write 16-bit PCM WAV from interleaved stereo samples (export utility). */
int  wb_wav_write_pcm16(const char *path, const wb_sample *data, uint32_t frames,
                        uint8_t channels, uint32_t sample_rate);

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

/* R025: real video-edit operations (Premiere-equivalent semantics). */
/* Ripple delete: remove clip, shift later clips left, timeline shrinks. */
int  wb_session_ripple_delete_video_clip(wb_session *s, int track, int clip);
/* Slip: slide clip's source in-point without moving it on the timeline. */
int  wb_session_slip_video_clip(wb_session *s, int track, int clip, double delta);
/* Roll: slide the cut between clip and the next clip (total duration fixed). */
int  wb_session_roll_video_clip(wb_session *s, int track, int clip, double delta);
/* G17 Slide: move clip in time; adjacent neighbors absorb the gap/overlap. */
int  wb_session_slide_video_clip(wb_session *s, int track, int clip, double delta);
/* G31: FX rack model ops — set/clear a slot's unit id; swap two slots. */
int  wb_session_set_insert(wb_session *s, int track, int slot, const char *unit_id);
int  wb_session_move_insert(wb_session *s, int track, int from, int to);
/* G63: re-pair facing fades of adjacent clips after moves/trims (dynamic
 * transitions). Pass the engine's clip-edit side-table. */
void wb_session_update_transitions(wb_session *s, struct wb_clip_edit_table *et);
/* G18: replace edit — same slot, new source (match frame). */
int  wb_session_replace_video_clip(wb_session *s, int track, int clip,
                                   const char *new_source);
/* G19: three-point edit — source in/dur placed at timeline dest, overwrite. */
int  wb_session_three_point_edit(wb_session *s, int track, const char *source,
                                 double src_in, double dur, double dest);

/* R048: transcript text-editing — delete the word range [w0, w1) from the
 * transcript AND ripple-cut that time span out of the track's video clips
 * (Descript's "delete words = delete media"). Returns 0 on success. */
int  wb_session_transcript_cut(wb_session *s, int track,
                               struct wb_transcript *tr, int w0, int w1);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_H */
