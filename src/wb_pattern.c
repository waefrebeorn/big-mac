/* wb_pattern.c — R074 hop 179 (G-SF068): step pattern sequencer.
 * Bar-synced 16-step triggering, deterministic. Pure C11. */
#include "wbus/wbus_pattern.h"
#include <string.h>

void wb_pattern_init(wb_pattern_state *ps, uint8_t pitch, double bpm) {
    if (!ps) return;
    memset(ps, 0, sizeof *ps);
    ps->pat.pitch = pitch;
    ps->pat.bpm = bpm > 1.0 ? bpm : 120.0;
    ps->last_fired_step = -1;
    ps->active = 1;
}

void wb_pattern_set_step(wb_pattern_state *ps, int step, uint8_t vel) {
    if (!ps || step < 0 || step >= WB_PATTERN_STEPS) return;
    ps->pat.steps[step] = vel;
}

int wb_pattern_fire(wb_pattern_state *ps, double pos, uint32_t n,
                    uint32_t sr, int *out_step, uint8_t *out_vel,
                    int out_cap) {
    if (!ps || !ps->active || !out_step || !out_vel) return -1;
    /* seconds per step: 16 steps per bar, 4 beats/bar */
    double spb = 60.0 / ps->pat.bpm / 4.0;      /* sec per 16th */
    double s0 = pos / (double)sr;
    double s1 = (pos + n) / (double)sr;
    int step0 = (int)(s0 / spb);
    int step1 = (int)(s1 / spb);   /* exclusive-ish boundary */
    int count = 0;
    for (int st = step0; st < step1 && count < out_cap; st++) {
        int idx = st % WB_PATTERN_STEPS;
        if (st < 0) continue;
        uint8_t vel = ps->pat.steps[idx];
        if (!vel) continue;
        /* avoid double-fire on the same global step across calls */
        if ((double)st == ps->last_fired_step &&
            (double)s0 <= ps->last_fired_step * spb + 1e-9) continue;
        out_step[count] = idx;
        out_vel[count] = vel;
        count++;
        ps->last_fired_step = (double)st;
    }
    return count;
}

void wb_pattern_clear_all(wb_pattern_state *ps) {
    if (!ps) return;
    memset(ps->pat.steps, 0, WB_PATTERN_STEPS);
}
