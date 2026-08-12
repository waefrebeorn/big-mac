#ifndef WUBUS_WBUS_H
#define WUBUS_WBUS_H

/* Big Mac DAW — public API.
 * Sample-accurate, C11, zero-third-party audio workstation.
 * Pull-based engine: the backend requests blocks; the engine renders them.
 * UI and engine talk through a lock-free command queue (see wb_cmd.h).
 */

#include <stdint.h>
#include <stddef.h>

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
    int      type;            /* 0 = MIDI/notes, 1 = audio */
    double   start;           /* sample position on timeline */
    double   length;          /* samples */
    uint32_t note_count;
    wb_note *notes;           /* MIDI clips */
    /* audio clip: owned buffer */
    int      audio_channels;
    uint32_t audio_frames;
    wb_sample *audio_data;
} wb_clip;

/* ---- mixer insert (one plugin slot on a track) ------------------------ */
typedef struct wb_plugin_slot {
    char   id[64];            /* plugin type id, e.g. "synth", "comp", "reverb" */
    void  *unit;              /* owned by engine */
} wb_plugin_slot;

/* ---- track ------------------------------------------------------------ */
typedef struct wb_track {
    char       name[64];
    int        kind;          /* 0 = instrument (has synth on first insert), 1 = audio */
    float      volume;        /* linear gain */
    float      pan;           /* -1..1 */
    int        mute;
    int        solo;
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
wb_track   *wb_session_add_track(wb_session *s, const char *name, int kind);
int         wb_session_add_note(wb_track *tr, double start, double dur, int pitch, int vel);

/* ---- session file save/load (.wbus text format) ----------------------- */
int  wb_session_save(const wb_session *s, const char *path);
wb_session *wb_session_load(const char *path);

/* ---- automation envelopes ---------------------------------------------- */
wb_automation_lane *wb_automation_lane_create(const char *param);
void  wb_automation_lane_destroy(wb_automation_lane *l);
int   wb_automation_add_point(wb_automation_lane *l, double time, double value, int curve);
int   wb_automation_clear(wb_automation_lane *l);
double wb_automation_value_at(const wb_automation_lane *l, double pos, double fallback);
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

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_H */
