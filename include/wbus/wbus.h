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
#include "wbus_text_edit.h"
#include "wbus_import.h"
#include "wbus_sonogram.h"
#include "wbus_midi_remote.h"

/* Forward declarations for cross-referenced types. */
typedef struct wb_mod_matrix wb_mod_matrix;

#ifdef __cplusplus
extern "C" {
#endif

#define WB_SAMPLE_RATE 44100
/* R074 fix: build version for CLI outputs. */
#define WB_VERSION "R074-fix1"
#define WB_MAX_CHANNELS 2
#define WB_MAX_BLOCK 4096
#define WB_MAX_TRACKS 128
#define WB_MAX_INSERT_SLOTS 8

typedef float wb_sample;

#include "wbus_ai_mix.h"

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
    /* G91: expression-map articulation index applied to this note.
     * -1 = none, else index into the active expression map's articulation
     * table. Set by wb_session_apply_articulation_to_note(). */
    int articulation; /* articulation index, -1 = none */
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
#define WB_TRACK_KIND_FOLDER 4   /* folder track (groups child tracks) */

/* ---- track ------------------------------------------------------------ */
typedef struct wb_track {
    char       name[64];
    int        kind;          /* WB_TRACK_KIND_* */
    float      volume;        /* linear gain */
    float      pan;           /* -1..1 */
    /* G37: spatial placement for the surround monitor path */
    double     spatial_angle; /* degrees 0..359, 0 = front center */
    float      spatial_gain;  /* elevation/height trim 0..2 */
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
    /* Track folders: folder_idx = index of the folder this track belongs to
     * (-1 = top-level, not in any folder). For folder tracks themselves,
     * folder_idx = their own index (self-referential). */
    int folder_idx;
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

/* ---- time signature changes -------------------------------------------- */
/* A time signature change on the timeline. Each change takes effect at its
 * sample position and holds until the next change. The session's base
 * time_sig_num/den applies before the first change. */
typedef struct wb_time_sig_change {
    double pos;        /* sample position where this time sig takes effect */
    int    num;        /* numerator (beats per bar), 1..32 */
    int    den;        /* denominator (beat unit), power of 2: 1,2,4,8,16,32 */
} wb_time_sig_change;

#define WB_MAX_TIME_SIG_CHANGES 64

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
    /* G79: 0 = meters pre-fader (default), 1 = post-fader (Pro Tools mode). */
    int       meter_post_fader;
    /* G37: surround monitor path enable (angles fold to stereo out) */
    int       surround_monitor;
    /* G04/G70 (Wave 3 lane C): app-level media bin — the persistent list of
     * imported assets. Each entry records the source path, kind, duration and
     * display name; `offline` is set on load when the source is missing. */
    uint32_t  bin_count;
#define WB_MAX_BIN 256
    wb_bin_entry bin_entries[WB_MAX_BIN];
    /* Time signature changes on the timeline (sorted by pos). The session's
     * base time_sig_num/den applies before the first change. */
    uint32_t  time_sig_change_count;
    wb_time_sig_change time_sig_changes[WB_MAX_TIME_SIG_CHANGES];
    /* Track folders: parallel array of folder metadata. Each folder is also a
     * track (kind == WB_TRACK_KIND_FOLDER); the metadata here tracks parent
     * folder, mute/solo/collapsed state, and child track membership. */
    uint32_t  folder_count;
#define WB_MAX_FOLDERS 64
    struct {
        int parent_folder_idx; /* -1 = top-level folder */
        int collapsed;         /* 1 = collapsed in UI */
        int track_list[WB_MAX_TRACKS]; /* indices of child tracks */
        int track_count;
    } folders[WB_MAX_FOLDERS];
    /* Linked track groups: Ableton-style simultaneous multi-track editing.
     * Each group is a collection of track indices that share edit operations
     * (move/trim/delete/add-note on clips at the same index). */
    uint32_t  link_group_count;
#define WB_MAX_LINK_GROUPS 16
    struct {
        int track_list[WB_MAX_TRACKS]; /* indices of member tracks */
        int track_count;
    } link_groups[WB_MAX_LINK_GROUPS];
    /* G91: expression maps — Cubase-style per-note articulation switching
     * for orchestral libraries. Up to 16 maps, each with up to 32 articulations. */
#define WB_MAX_EXPRESSION_MAPS 16
#define WB_MAX_ARTICULATIONS_PER_MAP 32
#define WB_MAX_TRACK_LANES 128
    uint32_t expr_map_count;
    struct {
        char name[64];
        uint32_t articulation_count;
        struct {
            char name[64];
            int midi_channel;  /* 1-16 */
            int cc_number;     /* 0-127, -1 = none */
            int cc_value;      /* 0-127 */
            int keyswitch;     /* MIDI note number, -1 = none */
        } articulations[WB_MAX_ARTICULATIONS_PER_MAP];
        int active_articulation; /* index of currently active articulation, -1 = none */
    } expr_maps[WB_MAX_EXPRESSION_MAPS];
    /* Per-track expression lane: which expression map is active for each track.
     * -1 = no expression map assigned. */
    int track_expr_lane[WB_MAX_TRACK_LANES];
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

/* ---- time signature changes -------------------------------------------- */
/* Add a time signature change at a sample position. Keeps the array sorted
 * by pos. Returns the new change's index, or -1 on error (invalid sig or
 * array full). */
int  wb_session_add_time_sig_change(wb_session *s, double pos_samples, int num, int den);
/* Remove the change at `index`. Returns 0 on success, -1 on bad index. */
int  wb_session_remove_time_sig_change(wb_session *s, int index);
/* Get the effective time signature at a sample position (binary search for
 * the most recent change at or before pos_samples; falls back to the
 * session's base time_sig_num/den). Returns 0 on success. */
int  wb_session_get_time_sig_at(wb_session *s, double pos_samples, int *num_out, int *den_out);
/* Number of time sig changes currently in the session. */
int  wb_session_time_sig_change_count(const wb_session *s);
/* Read back the i-th change. Returns 0 on success, -1 on bad index. */
int  wb_session_get_time_sig_change(const wb_session *s, int index, double *pos_out, int *num_out, int *den_out);
/* Get the sample position where `bar_number` begins (0-based), accounting
 * for all time sig changes. Returns the position in samples. */
double wb_session_get_bar_start(const wb_session *s, int bar_number);
/* Get the bar number (0-based) at a sample position. */
int  wb_session_get_bar_at(const wb_session *s, double pos_samples);
/* Convert samples to beats (quarter notes) at the given sample position
 * (uses the time sig at that position for the beat unit). */
double wb_session_samples_to_beats(const wb_session *s, double samples);
/* Convert beats (quarter notes) to samples at the given beat position
 * (uses the time sig at the corresponding sample for the beat unit). */
double wb_session_beats_to_samples(const wb_session *s, double beats);
int          wb_session_remove_track(wb_session *s, uint32_t idx); /* G09 */
int          wb_session_move_track(wb_session *s, uint32_t idx, int delta); /* G09 */
int         wb_session_add_note(wb_track *tr, double start, double dur, int pitch, int vel);
/* R074 hop 151 (G-SF071): per-track pan automation. The track's pan
 * follows the keyframable track (values -1..1) at render time; the
 * static tr->pan is the base when no keys fire. Engine keeps ownership
 * of nothing — caller retains the track. */
void wb_session_track_pan_automation(wb_track *tr, struct wb_param_track *pt);
struct wb_param_track;
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

/* ---- track folders ------------------------------------------------------ */
/* Create a folder track. parent_folder_idx=-1 for top-level. Returns the
 * track index of the new folder, or -1 on error. */
int wb_session_create_folder(wb_session *s, const char *name, int parent_folder_idx);
/* Add a track to a folder. Returns 0 on success, -1 on error. */
int wb_session_add_track_to_folder(wb_session *s, int track_idx, int folder_idx);
/* Remove a track from a folder (moves it to top-level). Returns 0 on success. */
int wb_session_remove_track_from_folder(wb_session *s, int track_idx, int folder_idx);
/* Set folder collapsed state (UI). collapsed=1 to collapse. */
int wb_session_set_folder_collapsed(wb_session *s, int folder_idx, int collapsed);
/* Set folder mute. When muted, all child tracks are muted too. */
int wb_session_set_folder_mute(wb_session *s, int folder_idx, int mute);
/* Set folder solo. When soloed, all child tracks are soloed too. */
int wb_session_set_folder_solo(wb_session *s, int folder_idx, int solo);
/* Get the number of tracks in a folder. */
int wb_session_get_folder_track_count(wb_session *s, int folder_idx);
/* Fill track_indices with the track indices in a folder. Returns count filled. */
int wb_session_get_folder_tracks(wb_session *s, int folder_idx, int *track_indices, int max_count);
/* Remove a folder, moving its children to the folder's parent (or top-level
 * if the folder was top-level). Returns 0 on success. */
int wb_session_remove_folder(wb_session *s, int folder_idx);

/* ---- bus routing matrix ------------------------------------------------- */
/* Create a bus track (kind=WB_TRACK_KIND_BUS). Returns track index or -1. */
int wb_session_create_bus(wb_session *s, const char *name);
/* Route a track's output to a bus (dest_idx) or master (dest_idx=-1). */
int wb_session_route_track_to(wb_session *s, int track_idx, int dest_idx);
/* Configure an aux send: src_track sends to dest_track at level, using
 * send_index 0 (SEND A) or 1 (SEND B). */
int wb_session_set_send(wb_session *s, int src_track, int dest_track, float level, int send_index);
/* Set a send's pre/post-fader tap. pre=1 taps before fader, pre=0 after. */
int wb_session_set_send_pre_fader(wb_session *s, int src_track, int send_index, int pre);

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
/* R073: output clip latch — stays set once true-peak exceeds full scale
 * until wb_engine_clear_clip_latch(). */
/* R073 hop 39: master bus brickwall limiter toggle. */
void wb_engine_set_master_limiter(wb_engine *e, int on);
int  wb_engine_get_clip_latch(const wb_engine *e);
void wb_engine_clear_clip_latch(wb_engine *e);
/* R073 hop 15: estimate an audio clip's tempo (60..180 BPM), 0 if unsure. */
double wb_session_estimate_bpm(const wb_session *s, int track, int clip);
/* R073 hop 91: snap a timeline time to the nearest beat of the session
 * grid (session BPM, beat origin at 0). Returns t unchanged if bpm<=0. */
double wb_session_snap_to_beat(double t, double bpm);
/* R073 hop 19: beat-grid phase for a clip of known tempo — [0, 60/bpm).
 * -1 on error. */
double wb_session_beat_phase(const wb_session *s, int track, int clip,
                             double bpm);
/* R073 hop 21: estimate the meter in beats per bar (2/3/4/6), 0 if unsure. */
int wb_session_estimate_meter(const wb_session *s, int track, int clip,
                              double bpm);
/* R073 hop 26: nearest zero-crossing to a proposed edit point. */
uint32_t wb_session_snap_zero_crossing(const wb_session *s, int track,
                                       int clip, uint32_t pos,
                                       uint32_t max_search);
/* R073 hop 27: split an audio clip at a zero-crossing-snapped point. */
int wb_session_split_audio_clip(wb_session *s, int track, int clip,
                                double split_secs);
/* R073 hop 28: equal-power fade-in/out at a clip's edges, in place. */
void wb_session_edge_fades(wb_session *s, int track, int clip,
                           double fade_ms);
/* R073 hop 29: peak-normalize a clip to `target` (e.g. 0.891 = -1 dBFS).
 * Returns applied gain or -1 on error. */
float wb_session_normalize(wb_session *s, int track, int clip,
                           float target);
/* R073 hop 31: loudness-normalize a clip to a BS.1770 integrated LUFS
 * target with a true-peak guard. Returns gain in dB, or -999 on error. */
/* R073 hop 48: render a child session and commit it as an audio clip in the
 * parent (Vegas-style "flatten nested sequence"). */
/* R073 hop 54: keyframable speed ramps — bind a speed curve to a clip;
 * retime_source_time integrates it into a source-time mapping. */
int wb_session_set_retime_ramp(struct wb_clip_edit_table *et, int track,
                               int clip, struct wb_param_track *speed);
double wb_session_retime_source_time(const struct wb_clip_edit_table *et,
                                     int track, int clip,
                                     double tl_offset);
int wb_session_bounce_sequence(struct wb_engine *e, struct wb_session *parent,
                               int track, double dest,
                               struct wb_session *child);

float wb_session_normalize_loudness(wb_session *s, int track, int clip,
                                    double target_lufs);
/* R073 hop 33: 4x-oversampled inter-sample true peak of a clip. */
float wb_session_true_peak(const wb_session *s, int track, int clip);
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
/* G79: toggle meter tap point session-wide. */
void wb_session_set_meter_point(wb_session *s, int post_fader);
/* G05/G06: audio/MIDI input recording. A lock-free input ring captures
 * incoming samples (fed by the app's audio-input device or by tests);
 * commit writes the captured span into a new audio clip at `dest`. */
typedef struct wb_input_ring wb_input_ring;
wb_input_ring *wb_inputring_create(uint32_t cap_frames);
void           wb_inputring_destroy(wb_input_ring *r);
uint32_t       wb_inputring_write(wb_input_ring *r, const wb_sample *data,
                                  uint32_t frames);   /* interleaved stereo */
uint32_t       wb_inputring_read(wb_input_ring *r, wb_sample *out,
                                 uint32_t frames);
uint32_t       wb_inputring_count(const wb_input_ring *r);
/* Commit the last `frames` captured samples as a new audio clip at `dest`. */
int            wb_inputring_commit_clip(wb_input_ring *r, struct wb_session *s,
                                        int track, double dest,
                                        uint32_t frames);

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
/* G77: copy channel-strip settings (volume/pan/insert chain) between tracks. */
int  wb_session_copy_strip(wb_session *s, int src_track, int dst_track);
/* G73: batch transitions — set a default crossfade across all cuts. */
int  wb_session_batch_transitions(wb_session *s, int track,
                                  struct wb_clip_edit_table *et, double xf);
/* R073 hop 93: batch transitions with cut midpoints snapped to the
 * session BPM grid (cut-on-beat). Returns cuts placed or -1. */
int  wb_session_batch_transitions_beat(wb_session *s, int track,
                                  struct wb_clip_edit_table *et, double xf);
/* R073 hop 95: batch transitions biased toward detected audio onsets of
 * a reference clip (atrack/aclip). Returns cuts placed or -1. */
int  wb_session_batch_transitions_onset(wb_session *s, int vtrack,
                                  struct wb_clip_edit_table *et, double xf,
                                  int atrack, int aclip);
/* G47: export the video arrangement as OpenTimelineIO (JSON). */
int  wb_session_export_otio(const wb_session *s, const char *path);
/* G27: transient detection — spectral-flux onsets over an audio clip.
 * Writes frame positions (samples, relative to clip start) into `out`
 * (capacity max). sensitivity 0..1 (0 = only huge hits). Returns the
 * number of transients found, or -1 on error. */
int wb_session_detect_transients(const wb_session *s, int track, int clip,
                                 float sensitivity,
                                 uint32_t *out, int max);
/* G26: WSOLA time-stretch + pitch-shift of interleaved audio.
 * rate > 1 = faster; semitones applied as post-resample. *outp is owned by
 * the caller. Returns output frame count, or 0 on error. */
uint32_t wb_timestretch(const wb_sample *in, uint32_t frames, uint32_t chn,
                        double rate, double semitones, wb_sample **outp);
/* R073-G26b: same, with input transient positions (from the G27 detector) —
 * WSOLA avoids splicing across them so attacks stay sharp. */
uint32_t wb_timestretch_tr(const wb_sample *in, uint32_t frames, uint32_t chn,
                           double rate, double semitones,
                           const uint32_t *trans, uint32_t ntrans,
                           wb_sample **outp);
/* R078: warp markers — Ableton-style elastic audio.
 * Remap source audio timeline to a musical beat timeline. */
typedef struct wb_warp wb_warp;
wb_warp *wb_warp_create(uint32_t sr);
void     wb_warp_destroy(wb_warp *w);
int      wb_warp_set_source(wb_warp *w, const wb_sample *audio,
                             uint32_t frames, uint32_t channels);
int      wb_warp_add_marker(wb_warp *w, double src_sample, double dst_beat);
int      wb_warp_remove_marker(wb_warp *w, int index);
int      wb_warp_clear_markers(wb_warp *w);
int      wb_warp_marker_count(const wb_warp *w);
double   wb_warp_src_to_dst(const wb_warp *w, double src_sample);
double   wb_warp_dst_to_src(const wb_warp *w, double beat);
int      wb_warp_auto_warp(wb_warp *w, const double *beat_positions, int num_beats);
void     wb_warp_process(wb_warp *w, double beat_start, double beat_end,
                          wb_sample *out, uint32_t frames);
/* G48: DAWproject-format export (core project.json body). */
int wb_session_export_dawproject(const wb_session *s, const char *path);
/* AAF/OMF interchange export (Pro Tools / Logic / Premiere interchange).
 * wb_aaf_export writes a simplified AAF-style XML/EDL hybrid;
 * wb_omf_export writes an OMF2 binary header + media refs + edit decisions.
 * Both return 0 on success, -1 on error (see wb_aaf_last_error()). */
int wb_aaf_export(const wb_session *session, const char *path);
int wb_omf_export(const wb_session *session, const char *path);
const char *wb_aaf_last_error(void);
/* G20: multicam — group video clips as angles and switch live. */
int wb_session_multicam_group(struct wb_clip_edit_table *et, int track,
                              const int *clip_indices, int n);
int wb_session_multicam_switch(struct wb_clip_edit_table *et, int track,
                               int any_member, int angle);
/* G37: surround/spatial monitoring — place a track on the surround field
 * (folded to the stereo bus as a monitor path) and toggle the monitor. */
int  wb_session_set_spatial(wb_session *s, int track, double angle_deg,
                            float elevation_gain);
void wb_session_spatial_enable(wb_session *s, int on);
/* R079: HRTF binaural 3D audio panner — Dolby Atmos-style positioning.
 * Simplified parametric HRTF: ITD (head radius), IID (head shadow),
 * pinna notch (elevation), air absorption (distance lowpass),
 * early reflections room model. Binaural mode = stereo L/R with
 * per-ear delay+gains; non-binaural = VBAP constant-power pan. */
void *wb_spatial_create(uint32_t sr);
void  wb_spatial_destroy(void *sp);
void  wb_spatial_set_position(void *sp, float azimuth, float elevation, float distance);
void  wb_spatial_set_listener_orientation(void *sp, float yaw, float pitch, float roll);
void  wb_spatial_process(void *sp, const wb_sample *in, wb_sample *out_l, wb_sample *out_r, uint32_t frames);
void  wb_spatial_set_binaural(void *sp, int enable);
void  wb_spatial_set_room(void *sp, float reverb_level, float room_size);
/* R080: object-based spatial audio panner (Dolby Atmos-style).
 * Up to 16 mono audio objects positioned in 3D space, rendered to
 * stereo binaural output via parametric HRTF (ITD + IID + distance + elevation). */
void *wb_atmos_create(uint32_t sr);
void  wb_atmos_destroy(void *a);
void  wb_atmos_set_position(void *a, int obj_id, float azimuth, float elevation, float distance);
void  wb_atmos_set_object_gain(void *a, int obj_id, float gain);
int   wb_atmos_process(void *a, const wb_sample **inputs, wb_sample **output_binaural,
                       int num_objects, uint32_t frames);
int   wb_atmos_get_object_count(const void *a);
/* G36: score view — diatonic staff position (0 = middle C line),
 * note-to-staff conversion, measure rendering, accidental/black-key checks. */
int  wb_score_note_to_staff(int midi_pitch, char *name_out, int cap, int *octave_out);
int  wb_score_pitch_to_line(int midi_pitch);
int  wb_score_staff_position(int midi_pitch);
int  wb_score_render_measure(wb_note *notes, int note_count, char *text_out, int cap);
int  wb_score_note_name(int midi_pitch, char *out, int cap);
int  wb_score_is_accidental(int midi_pitch);
int  wb_score_is_black_key(int midi_pitch);
/* G07: capture ingest — register one captured RGBA frame at `dest` and add
 * its source to the media bin. Hardware backends feed this; tests too. */
int wb_capture_frame(struct wb_session *s, int track, double dest,
                     const uint8_t *rgba, uint32_t w, uint32_t h);
/* G21: waveform auto-sync — sign-correlation offset between two audio clips.
 * Positive result means clip_b must move later to align with clip_a. */
int wb_session_sync_offset(const wb_session *s, int track_a, int clip_a,
                           int track_b, int clip_b, double *offset_secs);
/* G72: set a clip's retiming rate via its edit side-table entry. */
double wb_session_set_retime(struct wb_clip_edit_table *et, int track,
                             int clip, double rate);
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

/* ---- G91: expression maps (Cubase-style articulation management) ---- */
int  wb_session_add_expression_map(wb_session *s, const char *name);
int  wb_session_add_articulation(wb_session *s, int map_id, const char *name,
                                 int channel, int cc, int value, int keyswitch);
int  wb_session_set_active_articulation(wb_session *s, int map_id, int articulation_idx);
int  wb_session_get_articulation_count(wb_session *s, int map_id);
const char *wb_session_get_articulation_name(wb_session *s, int map_id, int idx);
int  wb_session_apply_articulation_to_note(wb_session *s, int track, int note_idx,
                                           int articulation_idx);
int  wb_session_set_expression_lane(wb_session *s, int track, int map_id);
int  wb_session_expression_map_count(wb_session *s);

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

/* R078: autocorrelation-based BPM detection from a raw audio buffer.
 * Returns detected BPM (60..200), or 0.0 on failure (silence/too short).
 * wb_tempo_detect_confidence() returns the last detection's confidence 0..1. */
float wb_tempo_detect(const wb_sample *audio, uint32_t frames, uint32_t sample_rate);
float wb_tempo_detect_confidence(void);

/* Audio-to-MIDI: monophonic pitch tracking via YIN + onset detection.
 * Converts an audio buffer to MIDI note events (wb_note array).
 * Only monophonic (single pitch at a time). */
typedef struct wb_audio_to_midi wb_audio_to_midi;

wb_audio_to_midi* wb_audio_to_midi_create(uint32_t sr);
void wb_audio_to_midi_destroy(wb_audio_to_midi *a);
int wb_audio_to_midi_convert(wb_audio_to_midi *a, const wb_sample *audio,
                              uint32_t frames, wb_note *out_notes,
                              int max_notes, int *out_count);
void wb_audio_to_midi_set_threshold(wb_audio_to_midi *a, float threshold);
void wb_audio_to_midi_set_min_duration(wb_audio_to_midi *a, float min_dur_ms);

/* ---- background (offline) render thread ---------------------------------- */
/* Format codes for wb_bg_render_start. */
#define WB_BG_FORMAT_WAV16  0   /* 16-bit PCM WAV */
#define WB_BG_FORMAT_WAV32F 1   /* 32-bit float WAV */
#define WB_BG_FORMAT_MP3    2   /* MP3 via ffmpeg */
#define WB_BG_FORMAT_MP4    3   /* MP4 (audio+black video) via ffmpeg */

typedef struct wb_bg_render wb_bg_render;

/* Start a background render thread. Returns a handle, or NULL on error.
 * `session` is caller-owned (referenced, not copied). `format` is one of
 * WB_BG_FORMAT_*. The render runs on a separate pthread; use poll/wait/cancel
 * to monitor or abort. */
wb_bg_render *wb_bg_render_start(const wb_session *session,
                                 const char *output_path, int format);

/* Poll for completion. Returns 1 if still running, 0 if done (success,
 * error, or cancelled). Writes current progress (0..1) to *progress_out
 * if non-NULL. */
int  wb_bg_render_poll(wb_bg_render *r, float *progress_out);

/* Block until done or timeout. Returns 0 on success, 1 on timeout, -1 on
 * error. NOTE: on success or error, this JOINS the thread and FREES the
 * handle — do not use `r` afterwards. On timeout, `r` remains valid. */
int  wb_bg_render_wait(wb_bg_render *r, int timeout_ms);

/* Signal a running render to cancel. The thread aborts at the next block
 * boundary; use wait() to join. */
void wb_bg_render_cancel(wb_bg_render *r);

/* Query status. Returns 0=pending, 1=running, 2=done, 3=error, 4=cancelled. */
int  wb_bg_render_status(wb_bg_render *r);

/* Return the error string if status==3 (error), else NULL. */
const char *wb_bg_render_error(wb_bg_render *r);

/* Force-cleanup a render (cancel + join + free). Use when you don't need
 * the result and want to tear down immediately. */
void wb_bg_render_destroy(wb_bg_render *r);

/* ---- drum rack (pad sampler) ------------------------------------------- */
typedef struct wb_drum_rack wb_drum_rack;

wb_drum_rack *wb_drum_rack_create(uint32_t sr);
void  wb_drum_rack_destroy(wb_drum_rack *r);
int   wb_drum_rack_load_pad(wb_drum_rack *r, int pad_index,
                            const wb_sample *audio, uint32_t frames,
                            uint32_t channels);
int   wb_drum_rack_load_pad_file(wb_drum_rack *r, int pad_index,
                                 const char *path);
int   wb_drum_rack_trigger(wb_drum_rack *r, int pad_index, float velocity);
int   wb_drum_rack_trigger_note(wb_drum_rack *r, int midi_note, float velocity);
void  wb_drum_rack_process(wb_drum_rack *r, wb_sample *out, uint32_t frames);
void  wb_drum_rack_set_pad_volume(wb_drum_rack *r, int pad, float vol);
void  wb_drum_rack_set_pad_pan(wb_drum_rack *r, int pad, float pan);
void  wb_drum_rack_set_pad_mute(wb_drum_rack *r, int pad, int mute);
void  wb_drum_rack_set_pad_solo(wb_drum_rack *r, int pad, int solo);
void  wb_drum_rack_set_master_volume(wb_drum_rack *r, float vol);
void  wb_drum_rack_clear(wb_drum_rack *r);
int   wb_drum_rack_pad_count(wb_drum_rack *r);

/* ---- MIDI scale quantizer -------------------------------------------- */
typedef struct wb_midi_scale wb_midi_scale;

enum {
    WB_SCALE_MAJOR = 0,
    WB_SCALE_MINOR,
    WB_SCALE_HARMONIC_MINOR,
    WB_SCALE_MELODIC_MINOR,
    WB_SCALE_DORIAN,
    WB_SCALE_PHRYGIAN,
    WB_SCALE_LYDIAN,
    WB_SCALE_MIXOLYDIAN,
    WB_SCALE_PENTATONIC_MAJOR,
    WB_SCALE_PENTATONIC_MINOR,
    WB_SCALE_BLUES,
    WB_SCALE_JAPANESE,
    WB_SCALE_FLAMENCO,
    WB_SCALE_WHOLE_TONE,
    WB_SCALE_DIMINISHED,
    WB_SCALE_CHROMATIC
};

wb_midi_scale *wb_midi_scale_create(void);
void           wb_midi_scale_destroy(void *ptr);
void           wb_midi_scale_set_root(void *ptr, int root_note);
void           wb_midi_scale_set_type(void *ptr, int type);
int            wb_midi_scale_snap(void *ptr, int midi_note);
int            wb_midi_scale_snap_up(void *ptr, int midi_note);
int            wb_midi_scale_snap_down(void *ptr, int midi_note);
int            wb_midi_scale_is_in_scale(void *ptr, int midi_note);
const char    *wb_midi_scale_get_name(int type);

/* ---- R079: MIDI generators ---- */
void *wb_midi_gen_create(uint32_t sr);
void  wb_midi_gen_destroy(void *gen);
void  wb_midi_gen_set_seed(void *gen, uint32_t seed);
int   wb_midi_gen_generate_melody(void *gen, int scale_root, int scale_type,
                                   int num_notes, int start_note, int range_semitones,
                                   uint32_t *seed_out, int *out_notes,
                                   int *out_positions, int *out_velocities,
                                   int *out_durations);
int   wb_midi_gen_generate_chords(void *gen, int scale_root, int scale_type,
                                    int num_chords, int progression_type,
                                    uint32_t *seed_out, int *out_roots,
                                    int *out_types);
int   wb_midi_gen_generate_rhythm(void *gen, int num_steps, int division,
                                    float density, float swing,
                                    uint32_t *seed_out, int *out_hits,
                                    int *out_velocities);

/* ---- AI melody composer ---------------------------------------------- */
void *wb_melody_ai_create(uint32_t sr);
void  wb_melody_ai_destroy(void *ai);
int   wb_melody_ai_compose(void *ai, int scale_root, int scale_type, int mood,
                            int num_bars, int ppq,
                            int *out_notes, int *out_positions,
                            int *out_durations, int *out_velocities,
                            int max_notes);
void  wb_melody_ai_set_tempo(void *ai, float bpm);
void  wb_melody_ai_set_mood(void *ai, int mood); /* 0=happy,1=sad,2=energetic,3=calm */
void  wb_melody_ai_set_range(void *ai, int min_note, int max_note);

/* ---- R079: AI chord progression generator ---- */
typedef struct wb_chord_ai wb_chord_ai;  /* struct defined in wb_chord_ai.c */
wb_chord_ai *wb_chord_ai_create(uint32_t sr);
void  wb_chord_ai_destroy(wb_chord_ai *ai);
int   wb_chord_ai_generate(wb_chord_ai *ai, int key, int mode, int num_chords,
                             int *out_roots, int *out_types, uint32_t *seed);
int   wb_chord_ai_generate_variation(wb_chord_ai *ai, uint32_t seed,
                                       int *out_roots, int *out_types);
void  wb_chord_ai_set_complexity(wb_chord_ai *ai, float complexity);
void  wb_chord_ai_set_mood(wb_chord_ai *ai, int mood);
float wb_chord_ai_get_tension(const wb_chord_ai *ai);

/* ---- R079: stem separation ---- */
int wb_stem_split(const wb_sample *mix, uint32_t frames, uint32_t chn,
                   wb_sample *vocals, wb_sample *drums, wb_sample *bass, wb_sample *other);

/* ---- R079: auto-reframe ---- */
typedef struct wb_autoreframe {
    int src_w, src_h, dst_w, dst_h;
    int mode;
    float smoothing;
    int sub_x, sub_y, sub_w, sub_h;
    float crop_x, crop_y;
    int initialized;
} wb_autoreframe;
int wb_autoreframe_init(wb_autoreframe *ar, int src_w, int src_h, int dst_w, int dst_h);
int wb_autoreframe_process(wb_autoreframe *ar, const uint8_t *frame_rgba,
                            int src_w, int src_h, uint8_t *out_rgba, int dst_w, int dst_h);
void wb_autoreframe_set_subject(wb_autoreframe *ar, int x, int y, int w, int h);
void wb_autoreframe_set_mode(wb_autoreframe *ar, int mode);
void wb_autoreframe_set_smoothing(wb_autoreframe *ar, float smoothing);

/* ---- R079: advanced dynamics ---- */
void *wb_dynamics_create(uint32_t sr);
void  wb_dynamics_destroy(void *d);
void  wb_dynamics_set_mode(void *d, int mode);
void  wb_dynamics_set_band_count(void *d, int bands);
void  wb_dynamics_set_band_freq(void *d, int band, float freq);
void  wb_dynamics_set_threshold(void *d, int band, float db);
void  wb_dynamics_set_ratio(void *d, int band, float ratio);
void  wb_dynamics_set_attack(void *d, int band, float ms);
void  wb_dynamics_set_release(void *d, int band, float ms);
void  wb_dynamics_set_knee(void *d, int band, float db);
void  wb_dynamics_set_parallel_mix(void *d, float mix);
void  wb_dynamics_set_sidechain_source(void *d, const wb_sample *ext, uint32_t frames);
void  wb_dynamics_set_sidechain_eq(void *d, float freq, float q, float gain);
void  wb_dynamics_process(void *d, wb_sample *out, const wb_sample *in, uint32_t frames);

/* ---- R079: Lottie renderer ---- */
void *wb_lottie_create(void);
void  wb_lottie_destroy(void *ptr);
int   wb_lottie_load_json(void *ptr, const char *json_str, int json_len);
int   wb_lottie_load_file(void *ptr, const char *path);
int   wb_lottie_render_frame(void *ptr, uint8_t *rgba_out, int w, int h, float time_sec);
float wb_lottie_get_duration(const void *ptr);
int   wb_lottie_get_fps(const void *ptr);

/* ---- R078: project templates ---- */
int         wb_proj_template_count(void);
const char *wb_proj_template_get_name(int template_id);
const char *wb_proj_template_get_description(int template_id);
int         wb_proj_template_apply(wb_session *session, int template_id);

/* ---- R078: wavetable synthesizer ---- */
#define WT_TABLE_SIZE 2048
#define WT_MAX_TABLES 64
#define WT_MAX_VOICES 16
#define WT_MAX_UNISON 8
typedef struct wb_wavetable {
    uint32_t sr;
    int table_count, table_size;
    float *tables[WT_MAX_TABLES];
    float *mipmaps[WT_MAX_TABLES][8];
    int mip_levels[WT_MAX_TABLES];
    float position;
    int interp_mode, unison_voices;
    float unison_spread, filter_cutoff, filter_resonance, filter_z1;
    struct wt_voice { float phase, phase_inc; int active, midi_note; float velocity; } voices[WT_MAX_VOICES];
    int active_voices;
} wb_wavetable;
void *wb_wavetable_create(uint32_t sr);
void  wb_wavetable_destroy(void *inst);
int   wb_wavetable_generate_wavetable(void *inst, int table_count, int preset);
void  wb_wavetable_note(void *inst, int note, int vel);
void  wb_wavetable_set_position(void *inst, float pos);
void  wb_wavetable_set_interpolation(void *inst, int mode);
void  wb_wavetable_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_wavetable_set_unison(void *inst, int voices, float spread);
void  wb_wavetable_set_filter(void *inst, float cutoff, float resonance);

/* ---- R078: vocal/formant synthesizer ---- */
void *wb_vocal_synth_create(uint32_t sr);
void  wb_vocal_synth_destroy(void *inst);
void  wb_vocal_synth_speak(void *inst, const char *phonemes);
void  wb_vocal_synth_set_pitch(void *inst, int midi_note);
void  wb_vocal_synth_render(void *inst, wb_sample *out, uint32_t frames);
void  wb_vocal_synth_set_vowel(void *inst, float vowel_position);
void  wb_vocal_synth_set_breathiness(void *inst, float amount);

/* ---- MPE (MIDI Polyphonic Expression) synthesizer ---- */
void *wb_mpe_create(uint32_t sr);
void  wb_mpe_destroy(void *mpe);
void  wb_mpe_note_on(void *mpe, int channel, int note, int velocity);
void  wb_mpe_note_off(void *mpe, int channel, int note);
void  wb_mpe_set_pitch_bend(void *mpe, int channel, int note, float semitones);
void  wb_mpe_set_pressure(void *mpe, int channel, int note, float pressure);
void  wb_mpe_set_timbre(void *mpe, int channel, int note, float timbre);
int   wb_mpe_active_notes(const void *mpe);
void  wb_mpe_render(void *mpe, wb_sample *out, uint32_t frames);
void  wb_mpe_set_global_bend(void *mpe, float semitones);

/* ---- linked track groups (Ableton-style multi-track editing) ----------- */
/* Create a link group from an array of track indices. Returns the group id
 * (0..WB_MAX_LINK_GROUPS-1), or -1 on error (too many groups, bad indices). */
int wb_session_create_link_group(wb_session *s, const int *track_indices, int num_tracks);
/* Remove a link group by id. Returns 0 on success, -1 on bad group_id. */
int wb_session_remove_link_group(wb_session *s, int group_id);
/* Add a track to an existing link group. Returns 0 on success, -1 on error. */
int wb_session_add_to_link_group(wb_session *s, int group_id, int track_index);
/* Remove a track from a link group. Returns 0 on success, -1 on error. */
int wb_session_remove_from_link_group(wb_session *s, int group_id, int track_index);
/* Move the clip at (track,clip) index to new_start on ALL linked tracks.
 * Only affects tracks that have a clip at the given index. Returns 0 on
 * success, -1 on bad group_id. */
int wb_session_linked_move_clip(wb_session *s, int group_id, int track, int clip, double new_start);
/* Trim the clip at (track,clip) by delta_head/delta_tail on ALL linked tracks.
 * delta_head: positive shortens the head (moves start later).
 * delta_tail: positive shortens the tail (moves end earlier).
 * Only affects tracks that have a clip at the given index. */
int wb_session_linked_trim_clip(wb_session *s, int group_id, int track, int clip, double delta_head, double delta_tail);
/* Delete the clip at (track,clip) from ALL linked tracks that have one. */
int wb_session_linked_delete_clip(wb_session *s, int group_id, int track, int clip);
/* Add a note to the clip at (track,clip) on ALL linked tracks that have one. */
int wb_session_linked_add_note(wb_session *s, int group_id, int track, double start, double dur, int pitch, int vel);
/* Get the track indices in a link group. Fills track_indices_out[0..max_count-1].
 * Returns the number filled, or -1 on bad group_id. */
int wb_session_get_link_group(const wb_session *s, int group_id, int *track_indices_out, int max_count);
/* Return the number of active link groups. */
int wb_session_link_group_count(const wb_session *s);

/* ---- Spectral audio repair (denoise / declick / dehum / gain) ---- */
#include "wbus/wbus_spectral_edit.h"

/* ---- Spectral effects (resonator / blur / time-freeze) ---- */
#include "wbus/wbus_spectral_fx.h"

/* ---- Audio restoration suite (iZotope RX style) ---- */
/* All functions process mono float buffers. Return 0 on success, -1 on error. */

/* ---- R079: color grading (DaVinci Resolve style) ---- */
void *wb_color_grading_create(int width, int height);
void  wb_color_grading_destroy(void *cg);
int   wb_color_grading_process(void *cg, uint8_t *rgba, int width, int height);
void  wb_color_grading_set_lift(void *cg, float r, float g, float b);
void  wb_color_grading_set_gamma(void *cg, float r, float g, float b);
void  wb_color_grading_set_gain(void *cg, float r, float g, float b);
void  wb_color_grading_set_saturation(void *cg, float sat);
void  wb_color_grading_set_contrast(void *cg, float contrast);
void  wb_color_grading_set_temperature(void *cg, float temp);
void  wb_color_grading_set_tint(void *cg, float tint);
int   wb_color_grading_load_lut(void *cg, const char *path);

/* Spectral subtraction denoise. strength in [0,1]: 0 = no-op, 1 = max reduction.
 * Estimates noise profile from first 100ms, applies spectral subtraction. */
int wb_restoration_denoise(const float *in, float *out, int n, float strength);

/* Declick: detect transient spikes via median filter deviation, interpolate
 * affected samples. threshold is the deviation above which a sample is
 * considered a click (e.g. 0.05–0.3). */
int wb_restoration_declick(const float *in, float *out, int n, float threshold);

/* Declip: detect clipped samples (near ±threshold), reconstruct via cubic
 * Catmull-Rom interpolation from surrounding clean samples. */
int wb_restoration_declip(const float *in, float *out, int n, float threshold);

/* Dehum: notch filter at hum_freq (typically 50 or 60 Hz) and harmonics
 * up to ~4kHz. Uses cascaded 2nd-order IIR notch filters. */
int wb_restoration_dehum(const float *in, float *out, int n, float hum_freq);

/* Voice isolation: bandpass filter 300Hz-4kHz + spectral gating to suppress
 * non-voice content. strength in [0,1]: higher = more aggressive isolation. */
int wb_restoration_voice_isolate(const float *in, float *out, int n, float strength);

/* ---- Podcast production workflow -------------------------------------- */
/* Voice isolation, noise gating, loudness normalization, chapter detection.
 * Opaque wb_podcast struct; allocate via wb_podcast_alloc(). */
typedef struct wb_podcast wb_podcast;

struct wb_podcast *wb_podcast_alloc(void);
void wb_podcast_free(struct wb_podcast *pc);

/* Initialize a podcast context. sr = sample rate (e.g. 44100). */
int wb_podcast_init(wb_podcast *pc, uint32_t sr);

/* Voice isolation: bandpass 300Hz-4kHz + spectral gating.
 * strength 0.0 = bypass, 1.0 = maximum isolation. */
void wb_podcast_set_voice_isolation_strength(void *pc, float strength);
int  wb_podcast_process_voice(void *pc, const float *in, float *out, int n);

/* Noise gate: attenuate signal below threshold (linear amplitude, e.g. 0.01). */
int wb_podcast_process_noise_gate(void *pc, const float *in, float *out,
                                   int n, float threshold);

/* Loudness normalization: measure LUFS, apply gain to reach target_lufs
 * (e.g. -16.0 for podcasts). Modifies audio in-place. */
int wb_podcast_normalize_loudness(void *pc, float *audio, int n,
                                   float target_lufs);

/* Chapter detection: find silence gaps > 2 seconds.
 * Returns number of chapter times written to chapter_times_out (max max_chapters).
 * chapter_times_out is in seconds. */
int wb_podcast_detect_chapters(void *pc, const float *audio, int n, float sr,
                                double *chapter_times_out, int max_chapters);

/* ---- R078: auto-reframe / smart crop ----------------------------------- */
/* AI-powered video reframing for vertical/square output (Premiere/AutoFlip
 * style). Pure-C11, zero-third-party: uses brightness/motion saliency and
 * skin-color detection (YCrCb) instead of a neural net. */

#define WB_AR_MODE_CENTER        0  /* center-lock on source frame */
#define WB_AR_MODE_FACE_TRACK    1  /* track detected faces (skin color) */
#define WB_AR_MODE_ACTION_TRACK  2  /* track motion-based saliency */
#define WB_AR_MODE_RULE_OF_THIRDS 3 /* rule-of-thirds composition */

/* Max frame dimensions supported (kept generous but bounded). */
#define WB_AR_MAX_DIM 8192

/* ---- macro/parameter rack (Ableton Instrument Rack style) ---- */
typedef struct wb_rack wb_rack;

void *wb_rack_create(uint32_t sr, const char *name);
void  wb_rack_destroy(void *rack);
int   wb_rack_add_unit(void *rack, const char *unit_type);
int   wb_rack_remove_unit(void *rack, int index);
int   wb_rack_unit_count(const void *rack);
void  wb_rack_set_macro(void *rack, int macro_index, float value);
void  wb_rack_set_macro_name(void *rack, int macro_index, const char *name);
int   wb_rack_bind_param(void *rack, int macro_index, int unit_index,
                         int param_index, float min_val, float max_val);
void  wb_rack_process(void *rack, wb_sample *out, uint32_t frames);
void  wb_rack_note(void *rack, int note, int vel);
void  wb_rack_set_midi_in(void *rack, int enable);

/* ---- professional mastering chain (iZotope Ozone style) ---- */
void *wb_mastering_create(uint32_t sr);
void  wb_mastering_destroy(void *m);
void  wb_mastering_set_input_gain(void *m, float db);
void  wb_mastering_set_output_gain(void *m, float db);
void  wb_mastering_set_loudness_target(void *m, float lufs);
void  wb_mastering_set_stereo_width(void *m, float width);
void  wb_mastering_set_bass_mono(void *m, int enable);
void wb_mastering_process(void *m, wb_sample *out_l, wb_sample *out_r, uint32_t frames);
float wb_mastering_get_loudness(const void *m);
float wb_mastering_get_peak(const void *m);

/* Advanced mastering suite (iZotope Ozone style) — multiband compression,
 * stereo imaging, loudness maximization, dithering. */
void *wb_master_adv_create(uint32_t sr);
void  wb_master_adv_destroy(void *m);
void  wb_master_adv_set_input_gain(void *m, float db);
void  wb_master_adv_set_output_gain(void *m, float db);
void  wb_master_adv_set_loudness_target(void *m, float lufs);
void  wb_master_adv_set_stereo_width(void *m, float width);
void  wb_master_adv_set_bass_mono(void *m, int enable);
int   wb_master_adv_process(void *m, wb_sample *out_l, wb_sample *out_r,
                            const wb_sample *in_l, const wb_sample *in_r,
                            uint32_t frames);
float wb_master_adv_get_loudness(const void *m);
float wb_master_adv_get_peak(const void *m);

/* ---- plugin delay compensation (PDC) ---------------------------------- */
/* Standalone PDC engine: delays non-latency tracks to match the longest
 * track's plugin chain latency, keeping all tracks sample-aligned at the
 * mix bus. Zero third-party, ring-buffer based. */
typedef struct wb_pdc wb_pdc;

wb_pdc *wb_pdc_create(int num_tracks, uint32_t sr);
void    wb_pdc_destroy(wb_pdc *p);
void    wb_pdc_set_latency(wb_pdc *p, int track, int samples);
int     wb_pdc_get_delay(const wb_pdc *p, int track);
int     wb_pdc_get_max_latency(const wb_pdc *p);
void    wb_pdc_set_enabled(wb_pdc *p, int enabled);
int     wb_pdc_is_enabled(const wb_pdc *p);
void    wb_pdc_process(wb_pdc *p, wb_sample **buffers, int num_tracks, uint32_t frames);

/* Engine-facing PDC wrappers (full integration wires these to engine->pdc). */
int  wb_engine_set_plugin_latency(wb_engine *engine, int track, int slot, int latency_samples);
int  wb_engine_get_track_latency(wb_engine *engine, int track);
int  wb_engine_get_max_latency(wb_engine *engine);
void wb_engine_apply_pdc(wb_engine *engine, wb_sample **track_buffers, int num_tracks, uint32_t frames);
int  wb_engine_set_pdc_enabled(wb_engine *engine, int enabled);

/* ---- cloud project sync (local-filesystem backend with versioning) ---- */
/* Store projects under ~/bigmac_cloud/<project_name>/ with immutable
 * version_N.wbus files. Uses wb_session_save/load for serialization. */

/* Initialize the cloud storage directory. Returns 0 on success. */
int wb_cloud_init(void);

/* Save a new version of a project. Returns the new version number (0-based),
 * or -1 on error. */
int wb_cloud_save_project(const char *project_name, const wb_session *s);

/* Load a specific version of a project. If version < 0, loads the current
 * version. Returns a new session (caller owns), or NULL on error. Sets
 * *out_version to the loaded version number (pass NULL to ignore). */
wb_session *wb_cloud_load_project(const char *project_name, int version, int *out_version);

/* List all projects in the cloud. Fills names_out[0..max_count-1] with
 * project directory names (caller must free each with free()). Returns the
 * number of projects found, capped at max_count. */
int wb_cloud_list_projects(char **names_out, int max_count);

/* Delete a project and all its versions. Returns 0 on success. */
int wb_cloud_delete_project(const char *project_name);

/* Get the number of versions stored for a project. Returns 0 if not found. */
int wb_cloud_get_version_count(const char *project_name);

/* Restore a specific version as the current version. Copies the version file
 * to a new version slot (so history is preserved). Returns the new version
 * number, or -1 on error. */
int wb_cloud_restore_version(const char *project_name, int version);

/* Remove all but the latest N versions of a project. Keeps versions
 * [version_count - keep_count, version_count - 1]. Returns the number of
 * versions removed, or -1 on error. */
int wb_cloud_cleanup(const char *project_name, int keep_count);

/* ---- R079: multi-language subtitle translation ----
 * Dictionary-based subtitle caption translator (AI-subtitle-generator style).
 * Scans a transcript, matches common phrases against an in-memory dictionary,
 * and replaces them with translations in the selected target language.
 * Timing information is preserved (words keep their [start,end] spans); only
 * the displayed word text is replaced. No ML model — pure lookup table. */

typedef struct wb_subtitle_translate {
    wb_session *session;        /* associated session (context; may be NULL) */
    int         lang_index;     /* currently selected target language */
    wb_transcript *transcript;  /* internal transcript to translate */
    char        output[8192];  /* rendered translated output (NUL-terminated) */
    int         processed;     /* 1 after wb_subtitle_translate_process() */
} wb_subtitle_translate;

/* Initialize a subtitle translator with a session. Populates a default
 * transcript of common subtitle phrases. Returns 0 on success, -1 on error
 * (NULL session / allocation failure). */
int wb_subtitle_translate_init(wb_subtitle_translate *st, wb_session *session);

/* Select the target language by ISO 639-1 code (e.g. "es", "ja", "zh").
 * Returns 0 on success, -1 on unknown code. */
int wb_subtitle_translate_language(wb_subtitle_translate *st, const char *lang_code);

/* Run the translation: scan the internal transcript, match each word/phrase
 * against the dictionary for the currently selected language, and build the
 * translated output string (timing preserved in the transcript). Returns 0
 * on success, -1 on error (not initialized / no language set). */
int wb_subtitle_translate_process(wb_subtitle_translate *st);

/* Copy the translated output into `out` (NUL-terminated, capped at `cap`).
 * Returns 0 on success, -1 on error. */
int wb_subtitle_translate_get_output(const wb_subtitle_translate *st,
                                     char *out, int cap);

/* Number of supported languages (28). */
int wb_subtitle_translate_get_language_count(void);

/* Get the human-readable language name for index [0, count).
 * Returns NULL on out-of-range. */
const char *wb_subtitle_translate_get_language_name(int index);

/* ---- Session View (Ableton-style clip launcher) ----------------------- */
#define WB_LAUNCH_TRIGGER 0  /* play while held / until stopped */
#define WB_LAUNCH_GATE   1  /* play while active */
#define WB_LAUNCH_TOGGLE 2  /* flip play/stop state */
#define WB_QUANT_FREE 0     /* immediate launch */
#define WB_QUANT_1_4  1     /* quantize to next quarter note */
#define WB_QUANT_1_8  2     /* quantize to next eighth note */
#define WB_QUANT_1_16 3     /* quantize to next sixteenth note */

/* Opaque handle. Create with wb_session_view_create(session). */
typedef struct wb_session_view wb_session_view;

/* Arrangement log entry — records a clip launch/stop event. */
typedef struct {
    double   time_samples; /* session time of the event */
    int      track;        /* track index */
    int      clip_ref;     /* clip index on the track */
    int      active;       /* 1 = launch, 0 = stop */
} wb_arrangement_entry;

/* Lifecycle. */
wb_session_view *wb_session_view_create(wb_session *session);
void wb_session_view_destroy(wb_session_view *sv);

/* Slot management — wb_session* variants use a default per-session view. */
int wb_session_create_slot(wb_session *session, int track, int scene);
int wb_session_view_create_slot(wb_session_view *sv, int track, int scene);

/* Assign a clip to a slot (must exist first). */
int wb_session_view_set_slot_clip(wb_session_view *sv, int track, int scene, int clip_ref);

/* Clip launch / stop. */
int wb_session_launch_clip(wb_session *session, int track, int scene);
int wb_session_stop_clip(wb_session *session, int track);
int wb_session_view_launch_clip(wb_session_view *sv, int track, int scene);
int wb_session_view_stop_clip(wb_session_view *sv, int track);

/* Scene launch (all tracks in a row simultaneously) / stop all. */
int wb_session_launch_scene(wb_session *session, int scene);
int wb_session_stop_all(wb_session *session);
int wb_session_view_launch_scene(wb_session_view *sv, int scene);
int wb_session_view_stop_all(wb_session_view *sv);

/* Launch mode: 0=trigger, 1=gate, 2=toggle. */
int wb_session_set_clip_launch_mode(wb_session *session, int track, int mode);
int wb_session_view_set_clip_launch_mode(wb_session_view *sv, int track, int mode);

/* Quantize: 0=free, 1=1/4, 2=1/8, 3=1/16. */
int wb_session_set_clip_quantize(wb_session *session, int track, int quantize);
int wb_session_view_set_clip_quantize(wb_session_view *sv, int track, int quantize);

/* Query: clip_ref currently playing on a track, or lowest playing scene. */
int wb_session_get_playing_clip(const wb_session *session, int track);
int wb_session_get_playing_scene(const wb_session *session);
int wb_session_view_get_playing_scene(const wb_session_view *sv, int track);

/* Arrangement recording. */
int wb_session_record_to_arrangement(wb_session *session, int enable);
int wb_session_view_record_to_arrangement(wb_session_view *sv, int enable);
uint32_t wb_session_view_arr_log_count(const wb_session_view *sv);
const wb_arrangement_entry *wb_session_view_arr_log(const wb_session_view *sv, uint32_t idx);

/* Slot introspection. */
int wb_session_view_get_slot_clip(const wb_session_view *sv, int track, int scene);
int wb_session_view_slot_exists(const wb_session_view *sv, int track, int scene);
int wb_session_view_slot_playing(const wb_session_view *sv, int track, int scene);
int wb_session_view_get_slot_launch_mode(const wb_session_view *sv, int track, int scene);
int wb_session_view_get_slot_quantize(const wb_session_view *sv, int track, int scene);

/* Advance session time (for arrangement recording). */
void wb_session_view_advance_time(wb_session_view *sv, double delta_samples);

/* ---- GPU-accelerated particle system (wb_particle_gpu.c) ---- */

/* Create a particle system with up to max_particles (capped at 10000).
 * Returns opaque handle, or NULL on failure. */
void *wb_gpu_particle_create(uint32_t max_particles);

/* Destroy a particle system. */
void wb_gpu_particle_destroy(void *ps);

/* Emit `count` particles at position (x, y, z) with random velocity spread. */
void wb_gpu_particle_emit(void *ps, float x, float y, float z, int count);

/* Update particle physics by dt seconds (SSE2 vectorized, 4 at a time). */
void wb_gpu_particle_update(void *ps, float dt);

/* Render particles to an RGBA buffer (w×h pixels, 4 bytes per pixel).
 * 3D→2D perspective projection, additive or alpha blending. */
void wb_gpu_particle_render(void *ps, uint8_t *rgba_out, int w, int h);

/* Set gravity vector (gx, gy, gz). */
void wb_gpu_particle_set_gravity(void *ps, float gx, float gy, float gz);

/* Set particle lifetime range (min_sec, max_sec). */
void wb_gpu_particle_set_lifetime(void *ps, float min_sec, float max_sec);

/* Set start/end colors (ARGB format). Color interpolates start→end over lifetime. */
void wb_gpu_particle_set_colors(void *ps, uint32_t start_color, uint32_t end_color);

/* Set particle size range (min_size, max_size) in pixels. */
void wb_gpu_particle_set_size(void *ps, float min_size, float max_size);

/* Get current number of active (alive) particles. */
int wb_gpu_particle_get_active_count(const void *ps);

/* ---- video stabilization (phase-correlation motion estimation) ---- */

/* Create a stabilizer instance for frames of size width x height.
 * Returns opaque handle, or NULL on failure. */
void *wb_stabilize2_create(int width, int height);

/* Destroy a stabilizer instance. */
void wb_stabilize2_destroy(void *s);

/* Process a frame in-place. Estimates motion relative to the previous
 * frame, smooths the trajectory, and applies a corrective translation.
 * Returns 0 on success, -1 on error. */
int wb_stabilize2_process(void *s, uint8_t *frame_rgba, int width, int height);

/* Set smoothing strength (0.0 = none, 0.99 = maximum). Default 0.85. */
void wb_stabilize2_set_smoothing(void *s, float smoothing);

/* Set crop percentage (0.0 = none, 0.4 = 40%). Default 0.05.
 * Higher values hide more border artifacts but reduce frame size. */
void wb_stabilize2_set_crop(void *s, float crop_percent);

/* Reset motion history (clears accumulated translation and smoothing state). */
void wb_stabilize2_reset(void *s);

/* ---- audio analysis (broadcast/metering style, wb_analysis.c) ---- */

/* Simplified BS.1770-4 K-weighted integrated loudness (LUFS).
 * Cascades a +4 dB high-shelf pre-filter (1513 Hz) and a 60 Hz RLB
 * high-pass, then computes -0.691 + 10*log10(mean-square). Suitable for
 * live metering; not a full compliance measurement (no gating). */
int wb_analysis_loudness(const float *audio, int n, float sr, float *lufs_out);

/* Peak sample magnitude in dBFS (0 dBFS = full scale). */
int wb_analysis_peak(const float *audio, int n, float *peak_db);

/* RMS level in dBFS (sqrt of mean square). */
int wb_analysis_rms(const float *audio, int n, float *rms_db);

/* Crest factor = peak_dB - RMS_dB in dB. */
int wb_analysis_crest_factor(const float *audio, int n, float *crest_db);

/* FFT magnitude spectrum binned into num_bins octave-spaced bands (dB).
 * Reuses wb_fft internally. bins must hold num_bins floats. */
int wb_analysis_spectrum(const float *audio, int n, float *bins, int num_bins, float sr);

/* Phase correlation between two channels: -1..+1 (Pearson r).
 * +1 = mono-compatible, 0 = uncorrelated, -1 = anti-phase. */
int wb_analysis_phase_correlation(const float *l, const float *r, int n, float *correlation);

/* ---- modulation matrix (wb_mod_matrix.c) ---- */
/* Bitwig-style modular modulation routing. Routes modulation sources
 * (LFOs, envelope, MIDI controllers) to parameter destinations with
 * bipolar amount scaling. Max 64 concurrent routes. */

/* Source IDs */
#define WB_MOD_SRC_LFO1       0
#define WB_MOD_SRC_LFO2       1
#define WB_MOD_SRC_ENVELOPE   2
#define WB_MOD_SRC_VELOCITY   3
#define WB_MOD_SRC_MODWHEEL   4
#define WB_MOD_SRC_PITCHBEND  5
#define WB_MOD_SRC_AFTERTOUCH 6

/* LFO waveform shapes */
#define WB_MOD_LFO_SINE     0
#define WB_MOD_LFO_TRIANGLE 1
#define WB_MOD_LFO_SAW      2
#define WB_MOD_LFO_SQUARE   3

/* Create / destroy a modulation matrix. */
void *wb_mod_matrix_create(void);
void  wb_mod_matrix_destroy(void *mm);

/* Add a modulation route. Returns route ID (>0) on success, -1 on error. */
int   wb_mod_matrix_add_route(void *mm, int src, int dst, float amount);

/* Remove a route by ID. Returns 0 on success, -1 on error. */
int   wb_mod_matrix_remove_route(void *mm, int route_id);

/* Set a route's amount (-1.0..+1.0, clamped). */
void  wb_mod_matrix_set_amount(void *mm, int route_id, float amount);

/* Process: compute source values and apply to param_values[0..num_params-1]. */
void  wb_mod_matrix_process(void *mm, float *param_values, int num_params);

/* Current number of active routes. */
int   wb_mod_matrix_route_count(const void *mm);

/* Remove all routes. */
void  wb_mod_matrix_clear(void *mm);

/* Configure an LFO (lfo_idx: 0=LFO1, 1=LFO2). freq in Hz (0.1..20, clamped). */
void  wb_mod_matrix_set_lfo(void *mm, int lfo_idx, int waveform, float freq);

/* Trigger envelope (ADSR in seconds). */
void  wb_mod_matrix_note_on(void *mm, float a, float d, float s, float r);
void  wb_mod_matrix_note_off(void *mm);

/* Set MIDI controller state (all normalized 0..1, except pitchbend -1..1). */
void  wb_mod_matrix_set_midi(void *mm, float vel, float mw, float pb, float at);

/* Evaluate modulation at sample position */
void  wb_mod_matrix_eval(void *mm, uint32_t n, float sample_rate,
                           void (*cb)(void *ctx, int track, int slot, int param, float value01),
                           void *ctx);

/* ---- R079: video transition pack ---- */
typedef struct wb_transition wb_transition;
int wb_transition_init(wb_transition *t, int type, int src_w, int src_h);
int wb_transition_process(wb_transition *t, const uint8_t *from, const uint8_t *to, uint8_t *out, float progress);
void wb_transition_set_duration(wb_transition *t, float seconds);
void wb_transition_set_param(wb_transition *t, int param, float value);

/* ---- AI arrangement assistant ---- */
/* Auto-arrange clips into song structures (verse/chorus/bridge/etc). */
void *wb_arrange_ai_create(void);
void  wb_arrange_ai_destroy(void *a);
int   wb_arrange_ai_arrange(void *a, const char *style, int num_clips,
                            int *clip_durations, int *out_order, int *out_sections);
int   wb_arrange_ai_get_section_name(int section_id, char *name_out, int cap);
void  wb_arrange_ai_set_tempo(void *a, float bpm);

/* Section ids for wb_arrange_ai_get_section_name / out_sections */
#define WB_ARR_SECTION_INTRO     0
#define WB_ARR_SECTION_VERSE     1
#define WB_ARR_SECTION_CHORUS    2
#define WB_ARR_SECTION_BRIDGE    3
#define WB_ARR_SECTION_SOLO      4
#define WB_ARR_SECTION_OUTRO     5
#define WB_ARR_SECTION_BUILD     6
#define WB_ARR_SECTION_DROP      7
#define WB_ARR_SECTION_BREAKDOWN 8
#define WB_ARR_SECTION_HEAD      9

/* ---- R079: cloud collaboration ---- */
void *wb_cloud_collab_create(const char *room_name);
void  wb_cloud_collab_destroy(void *inst);
int   wb_cloud_collab_join(void *inst, const char *user_id);
int   wb_cloud_collab_leave(void *inst, const char *user_id);
int   wb_cloud_collab_apply_op(void *inst, const char *user_id, const char *op_json);
int   wb_cloud_collab_get_state(void *inst, char *json_out, int cap);
int   wb_cloud_collab_user_count(const void *inst);

#ifdef __cplusplus
}
#endif
#endif /* WUBUS_WBUS_H */
