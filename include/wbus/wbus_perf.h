/* wbus_perf.h — live performance layer: decks, capture, replay (R065).
 *
 * The video-DJ engine. A PERFORMANCE is:
 *   - N DECKS, each holding a visual (mesh + color + motion params)
 *   - an EVENT LIST of timed actions: FIRE (deck on/off), FADE (crossfade
 *     position), PARAM (deck parameter tweak)
 * - LIVE mode: user/agent fires events in real time; events are captured
 *   while RECORDING is armed.
 * - REPLAY: wb_perf_render_frame(t) deterministically reconstructs any
 *   frame from the event list alone — the recording IS the element, and it
 *   soft-renders into exports with zero media cost.
 *
 * Determinism rule: replay depends only on (events, initial deck states,
 * t). No wall clock, no randomness without a seeded hash.
 *
 * C11, opaque structs, self-contained. See docs/R065-performance-tab.md.
 */
#ifndef WUBUS_WBUS_PERF_H
#define WUBUS_WBUS_PERF_H

#include <stdint.h>
#include <stddef.h>
#include "wbus_rast.h"
#include "wbus_mesh.h"

struct wb_anim;
#ifdef __cplusplus
extern "C" {
#endif

#define WB_PERF_MAX_DECKS 16

typedef struct wb_perf wb_perf;

/* Event types (the recorded vocabulary). */
typedef enum {
    WB_PERF_FIRE = 1,     /* show a deck (p = fade position target) */
    WB_PERF_UNFIRE,       /* hide a deck */
    WB_PERF_FADE,         /* crossfade A/B to p (0..1) */
    WB_PERF_PARAM         /* deck param tweak: p = value, i2 = which param */
} wb_perf_event_type;

wb_perf *wb_perf_create(int width, int height);
void     wb_perf_free(wb_perf *p);

/* Deck setup. Each deck owns a copy of the mesh. */
int  wb_perf_add_deck(wb_perf *p, const wb_mesh *m,
                      uint8_t r, uint8_t g, uint8_t b);
int  wb_perf_deck_count(const wb_perf *p);

/* LIVE control — also captured when recording. Returns event id or -1. */
int  wb_perf_fire(wb_perf *p, int deck);
int  wb_perf_unfire(wb_perf *p, int deck);
int  wb_perf_fade(wb_perf *p, float pos);          /* 0=deck A, 1=deck B */
int  wb_perf_param(wb_perf *p, int deck, int which, float v);
/* G-SF094: deck driven by a wb_anim (caller-owned). */
void wb_perf_deck_set_anim(wb_perf *p, int deck, struct wb_anim *a);

/* ---- capture / replay --------------------------------------------------- */

/* Arm capture: subsequent live calls are recorded with timestamps relative
 * to NOW-zero (first event after arm defines t0; pass explicit times via
 * wb_perf_capture_clock for deterministic tests). */
void wb_perf_record_arm(wb_perf *p);
void wb_perf_record_stop(wb_perf *p);
int  wb_perf_recording(const wb_perf *p);
int  wb_perf_event_count(const wb_perf *p);

/* Snapshot the live event log (R068: for building perf-clip clips).
 * Returns a pointer into an internal buffer of exactly event_count()
 * entries; valid until the next mutating call on p. */
const void *wb_perf_event_dump(const wb_perf *p);
int  wb_perf_deck_fired(const wb_perf *p, int deck);
void wb_perf_reset_for_replay(wb_perf *p);

/* Public mirror of the internal event record. */
typedef struct {
    double t;
    int    type;          /* wb_perf_event_type */
    int    deck;
    float  v;
    int    which;
} wb_perf_event_view;

/* Set the performance clock (seconds). Live calls stamp with this. */
void wb_perf_set_clock(wb_perf *p, double t);

/* REPLAY: reset all deck states to "off", then apply every event with
 * e_time <= t. After this, wb_perf_render_frame draws the state at t. */
void wb_perf_seek(wb_perf *p, double t);

/* Render current state into RGBA (w*h*4, alpha-keyed). Decks are laid out
 * split-screen by fade: left half = even decks, right half = odd, or full-
 * screen when only one deck is fired. */
void wb_perf_render_frame(wb_perf *p, uint8_t *out_rgba);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_PERF_H */
