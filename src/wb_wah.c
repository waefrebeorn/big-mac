/* wb_wah.c — auto-wah / envelope filter for YTP funkiness.
 *
 * R080: Envelope-controlled bandpass — the classic "talking" filter sound.
 * Input amplitude sweeps the filter frequency, creating vocal-like resonances.
 *
 * Also supports:
 *   - LFO-driven wah (cyclic)
 *   - Pedal wah (manual frequency control)
 *   - Crybaby (fixed Q sweep)
 *   - Talking box (formant-like peaks)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef enum {
    WAH_AUTO = 0,       /* Envelope-driven */
    WAH_LFO,            /* Cyclic sweep */
    WAH_PEDAL,          /* Manual control */
    WAH_TALKING,        /* Formant-like */
    WAH_TYPES
} wah_mode_t;

typedef struct {
    uint32_t sr;
    wah_mode_t mode;

    /* Filter state (state-variable filter) */
    float lp, hp, bp;       /* SVF outputs */
    float freq;             /* Current center frequency */
    float freq_min;         /* Min frequency */
    float freq_max;         /* Max frequency */
    float q;                /* Resonance */

    /* Envelope follower */
    float env;
    float attack;
    float release;

    /* LFO */
    float lfo_phase;
    float lfo_rate;

    /* Pedal position (0-1) */
    float pedal;

    /* Talking mode: dual formant peaks */
    float formant1_freq;
    float formant2_freq;
    float formant1_amp;
    float formant2_amp;
} wb_wah_inst;

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void *wb_wah_create(uint32_t sr) {
    wb_wah_inst *inst = (wb_wah_inst *)calloc(1, sizeof(wb_wah_inst));
    if (!inst) return NULL;
    inst->sr = sr;
    inst->mode = WAH_AUTO;
    inst->freq_min = 200.0f;
    inst->freq_max = 3000.0f;
    inst->freq = 500.0f;
    inst->q = 5.0f;
    inst->attack = 0.01f;
    inst->release = 0.1f;
    inst->lfo_rate = 4.0f;
    inst->pedal = 0.5f;
    inst->formant1_freq = 700.0f;
    inst->formant2_freq = 1200.0f;
    inst->formant1_amp = 1.0f;
    inst->formant2_amp = 0.7f;
    return inst;
}

void wb_wah_destroy(void *inst) {
    free(inst);
}

void wb_wah_set_mode(void *inst, int mode) {
    wb_wah_inst *w = (wb_wah_inst *)inst;
    if (!w) return;
    if (mode < 0 || mode >= WAH_TYPES) mode = 0;
    w->mode = (wah_mode_t)mode;
}

void wb_wah_set_range(void *inst, float freq_min, float freq_max) {
    wb_wah_inst *w = (wb_wah_inst *)inst;
    if (!w) return;
    w->freq_min = clampf(freq_min, 50.0f, 5000.0f);
    w->freq_max = clampf(freq_max, w->freq_min + 100.0f, 10000.0f);
}

void wb_wah_set_q(void *inst, float q) {
    wb_wah_inst *w = (wb_wah_inst *)inst;
    if (w) w->q = clampf(q, 0.5f, 20.0f);
}

void wb_wah_set_pedal(void *inst, float pos) {
    wb_wah_inst *w = (wb_wah_inst *)inst;
    if (w) w->pedal = clampf(pos, 0.0f, 1.0f);
}

void wb_wah_set_lfo_rate(void *inst, float rate) {
    wb_wah_inst *w = (wb_wah_inst *)inst;
    if (w) w->lfo_rate = clampf(rate, 0.1f, 20.0f);
}

/* Process a mono buffer in-place */
void wb_wah_process(void *inst, float *buf, int n) {
    wb_wah_inst *w = (wb_wah_inst *)inst;
    if (!w) return;

    for (int i = 0; i < n; i++) {
        /* Determine target frequency based on mode */
        float target_freq;

        switch (w->mode) {
            case WAH_AUTO: {
                /* Envelope follower */
                float level = fabsf(buf[i]);
                float rate = (level > w->env) ? w->attack : w->release;
                w->env += (level - w->env) * rate;
                target_freq = w->freq_min + w->env * (w->freq_max - w->freq_min) * 50.0f;
                break;
            }
            case WAH_LFO: {
                w->lfo_phase += 2.0f * M_PI * w->lfo_rate / w->sr;
                if (w->lfo_phase > 2.0f * M_PI) w->lfo_phase -= 2.0f * M_PI;
                float lfo = (sinf(w->lfo_phase) + 1.0f) * 0.5f;
                target_freq = w->freq_min + lfo * (w->freq_max - w->freq_min);
                break;
            }
            case WAH_PEDAL:
                target_freq = w->freq_min + w->pedal * (w->freq_max - w->freq_min);
                break;
            case WAH_TALKING: {
                /* Blend between two formant positions based on envelope */
                float level = fabsf(buf[i]);
                float rate = (level > w->env) ? w->attack : w->release;
                w->env += (level - w->env) * rate;
                float blend = clampf(w->env * 30.0f, 0.0f, 1.0f);
                target_freq = w->formant1_freq * (1.0f - blend) + w->formant2_freq * blend;
                break;
            }
            default:
                target_freq = w->freq;
                break;
        }

        /* Smooth frequency changes */
        w->freq += (target_freq - w->freq) * 0.001f;
        w->freq = clampf(w->freq, 50.0f, 10000.0f);

        /* State-variable filter (Chamberlin) */
        float f = 2.0f * sinf(M_PI * w->freq / w->sr);
        float q_res = 1.0f / w->q;

        float in = buf[i];
        w->lp += f * w->bp;
        w->hp = in - w->lp - q_res * w->bp;
        w->bp += f * w->hp;

        /* Output: bandpass */
        buf[i] = w->bp;
    }
}

/* Preset: classic crybaby */
void wb_wah_preset_crybaby(void *inst) {
    wb_wah_set_mode(inst, WAH_AUTO);
    wb_wah_set_range(inst, 300.0f, 2500.0f);
    wb_wah_set_q(inst, 8.0f);
}

/* Preset: funky cyclic */
void wb_wah_preset_funk(void *inst) {
    wb_wah_set_mode(inst, WAH_LFO);
    wb_wah_set_range(inst, 400.0f, 2000.0f);
    wb_wah_set_q(inst, 6.0f);
    wb_wah_set_lfo_rate(inst, 3.5f);
}

/* Preset: talking box */
void wb_wah_preset_talking(void *inst) {
    wb_wah_set_mode(inst, WAH_TALKING);
    wb_wah_set_range(inst, 500.0f, 2500.0f);
    wb_wah_set_q(inst, 10.0f);
}
