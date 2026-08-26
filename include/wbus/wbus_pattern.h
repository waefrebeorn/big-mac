/* wbus_pattern.h — R074 hop 179 (G-SF068): step pattern sequencer.
 * 16-step patterns trigger note events bar-synced via the transport.
 * Pure data + evaluation; the engine drives it from block positions. */
#ifndef WUBUS_PATTERN_H
#define WUBUS_PATTERN_H

#include <stdint.h>
#include <stddef.h>

#define WB_PATTERN_STEPS 16

typedef struct {
    uint8_t  steps[WB_PATTERN_STEPS];   /* velocity per step, 0 = off */
    uint8_t  pitch;                     /* MIDI note */
    double   bpm;                       /* tempo this pattern runs at */
} wb_pattern;

typedef struct {
    wb_pattern pat;
    double     last_fired_step;   /* -1 = none */
    int        active;
} wb_pattern_state;

void wb_pattern_init(wb_pattern_state *ps, uint8_t pitch, double bpm);
void wb_pattern_set_step(wb_pattern_state *ps, int step, uint8_t vel);
/* Fire notes for the half-open sample range [pos, pos+n) at SR.
 * Writes (step_index, velocity) pairs into out; returns count. */
int  wb_pattern_fire(wb_pattern_state *ps, double pos, uint32_t n,
                     uint32_t sr, int *out_step, uint8_t *out_vel,
                     int out_cap);
void wb_pattern_clear_all(wb_pattern_state *ps);

#endif /* WUBUS_PATTERN_H */
