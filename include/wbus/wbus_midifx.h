#ifndef WUBUS_WBUS_MIDIFX_H
#define WUBUS_WBUS_MIDIFX_H

/* Big Mac DAW — MIDI FX chain (mf1).
 *
 * MIDI FX transform note events BEFORE they reach the track instrument, matching
 * Ableton's MIDI-effects layer (arpeggiator, chord, note repeat, velocity, etc).
 * Each unit receives a note on/off event and may emit zero or more output events.
 *
 * The engine holds a per-track midifx chain. Incoming WB_CMD_NOTE events are
 * passed through the chain; every event the chain emits is forwarded to the
 * instrument voice. Stateful units (arp) also receive a per-block clock tick.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A single MIDI note event flowing through the FX chain. */
typedef struct wb_midifx_event {
    uint8_t pitch;   /* MIDI note 0-127 */
    uint8_t vel;     /* 0-127 (0 = note-off) */
    int     on;      /* 1 = note-on, 0 = note-off */
    int     tick;    /* transport tick index this event belongs to (for arps) */
} wb_midifx_event;

typedef enum {
    WB_MIDIFX_NONE = 0,
    WB_MIDIFX_ARP,        /* arpeggiator: cycles held notes on a clock */
    WB_MIDIFX_CHORD,      /* builds a chord from each input note */
    WB_MIDIFX_TRANSPOSE,  /* shift pitch by N semitones */
    WB_MIDIFX_VELOCITY    /* scale velocity */
} wb_midifx_type;

/* A MIDI FX unit instance. */
typedef struct wb_midifx wb_midifx;

wb_midifx *wb_midifx_create(wb_midifx_type type);
void       wb_midifx_destroy(wb_midifx *m);
wb_midifx_type wb_midifx_get_type(const wb_midifx *m);

/* Set a unit parameter (0..1 normalized or raw depending on unit).
 * arp:   p0 = rate (steps/sec), p1 = pattern (0=up,1=down,2=updown)
 * chord: p0..p2 = semitone offsets for voices 1..3 (-24..+24)
 * trans: p0 = semitones (-24..+24)
 * vel:   p0 = scale 0..2 */
void wb_midifx_set_param(wb_midifx *m, int param, float value);

/* Transform one input event into 0..maxout output events.
 * Returns the number of output events written to `out`. */
int  wb_midifx_process(wb_midifx *m, const wb_midifx_event *in,
                       wb_midifx_event *out, int maxout);

/* Advance the unit's internal clock by `ticks` 1/16-note steps (arp uses this
 * to emit the next arpeggiated note; stateless units ignore it). Returns the
 * number of events generated this tick (written to `out`). */
int  wb_midifx_tick(wb_midifx *m, int ticks, wb_midifx_event *out, int maxout);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_MIDIFX_H */
