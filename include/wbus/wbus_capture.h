#ifndef WBUS_CAPTURE_H
#define WBUS_CAPTURE_H

#include <stdint.h>
#include <stddef.h>

/* file-scope forward decls: a tag first seen inside a prototype has BLOCK
 * scope in C11, creating an incompatible type (clang -Wvisibility trap) */
struct wb_session;
struct wb_engine;

#ifdef __cplusplus
extern "C" {
#endif

/* G93/G94 — capture machinery (R072 gaps).
 *
 * G93 capture-quantize: a rolling log of played notes with sample timestamps.
 * CAPTURE writes the last N seconds of logged notes into a NEW clip on the
 * chosen track, quantized to the 16th-note grid — jamming becomes material
 * without pre-arming (Ableton "Capture MIDI").
 *
 * G94 record-session-to-arrangement: while armed, poll the session launcher
 * (wb_engine_launched_clip) each frame; launch/stop transitions are logged as
 * spans. Committing writes each span's source MIDI clip content onto the
 * arrangement at the launch position (Bitwig "Record to Arranger"). */

typedef struct wb_capnote {
    double  t;      /* song position in SAMPLES when the note was played */
    int     track;
    uint8_t pitch;
    uint8_t vel;    /* 0 = note-off (logged for completeness) */
} wb_capnote;

typedef struct wb_caplog wb_caplog;

wb_caplog *wb_caplog_create(void);
void       wb_caplog_destroy(wb_caplog *l);
void       wb_caplog_note(wb_caplog *l, double t_samples, int track,
                          int pitch, int vel);
int        wb_caplog_count(const wb_caplog *l);
const wb_capnote *wb_caplog_at(const wb_caplog *l, int i); /* 0 = oldest */
void       wb_caplog_clear(wb_caplog *l);

/* G93: collect the notes played in [t_now - win, t_now] on `track`, quantize
 * each onset to the nearest 16th of the bpm grid, and write them into a NEW
 * MIDI clip appended to the track. Returns notes written, or -1 empty/error. */
int wb_capture_quantize(wb_caplog *l, struct wb_session *s, int track,
                        double t_now_samples, double win_secs, double bpm);

/* ---- G94: session-launcher recorder ------------------------------------ */

typedef struct wb_launch_span {
    int    track;
    int    clip_idx;
    double t_start;    /* samples */
    double t_stop;     /* samples (< t_start = still open) */
} wb_launch_span;

typedef struct wb_launchrec wb_launchrec;

wb_launchrec *wb_launchrec_create(void);
void          wb_launchrec_destroy(wb_launchrec *r);
void          wb_launchrec_start(wb_launchrec *r, const struct wb_session *s);
/* Poll once per frame. Returns transitions recorded this call (0 normally). */
int           wb_launchrec_poll(wb_launchrec *r, const struct wb_session *s,
                                const struct wb_engine *e, double t_now_samples);
void          wb_launchrec_finish(wb_launchrec *r, double t_now_samples);
int           wb_launchrec_span_count(const wb_launchrec *r);
const wb_launch_span *wb_launchrec_span(const wb_launchrec *r, int i);
/* Commit spans onto the arrangement (MIDI clips; looped to fill each span).
 * Returns clips placed, or -1 on error. */
int wb_launchrec_commit(wb_launchrec *r, struct wb_session *s);

#ifdef __cplusplus
}
#endif
#endif /* WBUS_CAPTURE_H */
