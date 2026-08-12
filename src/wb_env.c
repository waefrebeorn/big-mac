/* wb_env.c — ADSR envelope generator. */

#include "wbus_dsp.h"

void wb_env_init(wb_env *e, float sr) {
    e->sr = sr;
    e->level = 0.0;
    e->stage = 0;
    e->note_on = 0;
    e->a = e->d = e->s = e->r = 0.0;
    e->cur = 0.0;
}

void wb_env_note_on(wb_env *e, float a, float d, float s, float r) {
    e->a = a; e->d = d; e->s = s; e->r = r;
    e->stage = 1;           /* attack */
    e->cur = 0.0;
    e->note_on = 1;
}

void wb_env_note_off(wb_env *e) {
    if (e->stage > 0 && e->stage < 5) {
        e->stage = 4;       /* release */
        e->cur = 0.0;
    }
    e->note_on = 0;
}

float wb_env_process(wb_env *e) {
    float sr = e->sr;
    switch (e->stage) {
    case 0: /* idle */
        e->level = 0.0;
        break;
    case 1: /* attack: linear to 1 over a seconds */
        e->level += (1.0f / (e->a * sr + 1e-9f));
        if (e->level >= 1.0f) { e->level = 1.0f; e->stage = 2; e->cur = 0; }
        break;
    case 2: /* decay: exponential to sustain */
        if (e->d > 0) {
            e->level += (e->s - e->level) / (e->d * sr + 1e-9f);
            if (e->cur > e->d * sr) { e->level = e->s; e->stage = 3; }
        } else { e->stage = 3; e->level = e->s; }
        e->cur += 1.0;
        break;
    case 3: /* sustain */
        e->level = e->s;
        break;
    case 4: /* release: exponential to 0 */
        if (e->r > 0) {
            e->level *= 1.0f - (1.0f / (e->r * sr + 1e-9f));
            if (e->level < 1e-4f) { e->level = 0; e->stage = 0; }
        } else { e->level = 0; e->stage = 0; }
        break;
    default:
        break;
    }
    return (float)e->level;
}
