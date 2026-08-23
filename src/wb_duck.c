/* wb_duck.c — auto-ducking envelope generator (R063). */

#include "wbus/wbus_duck.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

wb_duck_params wb_duck_default_params(void) {
    wb_duck_params p;
    p.duck_amount = 0.75f;   /* music drops to 25% */
    p.threshold   = 0.05f;
    p.attack_ms   = 120.0f;
    p.release_ms  = 400.0f;
    return p;
}

/* helper: add a lane point (time in samples, value 0..1 gain multiplier) */
static void lane_pt(wb_automation_lane *lane, double t_samples, float v) {
    wb_automation_add_point(lane, t_samples, v, 0);
}

int wb_duck_generate(const wb_sample *voice, uint32_t n, uint32_t sr,
                     const wb_duck_params *params,
                     wb_automation_lane *lane) {
    if (!voice || !lane || n == 0 || sr == 0) return -1;
    wb_duck_params dp = params ? *params : wb_duck_default_params();
    if (dp.duck_amount < 0) dp.duck_amount = 0;
    if (dp.duck_amount > 1) dp.duck_amount = 1;

    const uint32_t block = sr / 50;            /* ~20ms blocks */
    if (block == 0) return -1;
    const uint32_t nblocks = n / block;
    if (nblocks < 2) return -1;

    /* 1. RMS envelope per block */
    float *rms = malloc(nblocks * sizeof(float));
    if (!rms) return -1;
    for (uint32_t b = 0; b < nblocks; b++) {
        double sum = 0;
        for (uint32_t i = 0; i < block; i++) {
            float v = voice[b*block + i];
            sum += (double)v*v;
        }
        rms[b] = sqrtf((float)(sum / block));
    }

    /* 2. gate with hysteresis: open at threshold, close at threshold*0.6
     * (prevents chatter on breathy consonants) */
    uint8_t *active = malloc(nblocks);
    if (!active) { free(rms); return -1; }
    int state = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        if (!state && rms[b] >= dp.threshold) state = 1;
        else if (state && rms[b] < dp.threshold*0.6f) state = 0;
        active[b] = (uint8_t)state;
    }

    /* 3. build the gain curve with attack/release ramps.
     * music_gain(t) = 1 when voice silent, (1-duck_amount) when speaking */
    const float ducked = 1.0f - dp.duck_amount;
    uint32_t atk_blocks = (uint32_t)(dp.attack_ms  * 0.001f * sr / block);
    uint32_t rel_blocks = (uint32_t)(dp.release_ms * 0.001f * sr / block);
    if (atk_blocks < 1) atk_blocks = 1;   /* avoid div-by-zero below */
    if (rel_blocks < 1) rel_blocks = 1;

    float *gain = malloc(nblocks * sizeof(float));
    if (!gain) { free(rms); free(active); return -1; }
    float g = 1.0f;
    for (uint32_t b = 0; b < nblocks; b++) {
        float target = active[b] ? ducked : 1.0f;
        uint32_t ramp = (target < g) ? atk_blocks : rel_blocks;
        float step = (target - g) / (float)ramp;
        g += step;
        /* snap to target when close enough */
        if (fabsf(target - g) < 0.01f) g = target;
        gain[b] = g;
    }

    /* 4. write lane points at every gain change of >=0.05 (keeps the lane
     * compact while preserving shape) */
    int points = 0;
    float last_written = -1.0f;
    for (uint32_t b = 0; b < nblocks; b++) {
        float gv = gain[b];
        if (last_written < 0 || fabsf(gv - last_written) >= 0.049f) {
            lane_pt(lane, (double)b*block, gv);
            last_written = gv;
            points++;
        }
    }
    /* final point so the hold extends to the end */
    lane_pt(lane, (double)nblocks*block, gain[nblocks-1]);
    points++;

    free(rms); free(active); free(gain);
    return points;
}
