/* wb_sampler.c — sample-playback instrument.
 * Plays back a short PCM sample (loopable) at MIDI-note-tuned pitch.
 * Loads a raw 16-bit mono sample; resamples by linear interpolation.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus.h"

typedef struct wb_sampler_voice {
    int active;
    int note;
    double pos;         /* fractional sample position */
    double rate;        /* playback rate */
    float vel;
} wb_sampler_voice;

typedef struct wb_sampler_inst {
    uint32_t sr;
    wb_sample *samples;
    uint32_t sample_count;
    int loop;
    wb_sampler_voice voices[16];
} wb_sampler_inst;

void *wb_sampler_create(uint32_t sr);
void  wb_sampler_destroy(void *inst);
void  wb_sampler_load(void *inst, const wb_sample *data, uint32_t count, int loop);
void  wb_sampler_note(void *inst, int note, int vel);
void  wb_sampler_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

void *wb_sampler_create(uint32_t sr) {
    wb_sampler_inst *s = calloc(1, sizeof(*s));
    s->sr = sr;
    return s;
}
void wb_sampler_destroy(void *inst) {
    wb_sampler_inst *s = inst;
    if (s) { free(s->samples); free(s); }
}
void wb_sampler_load(void *inst, const wb_sample *data, uint32_t count, int loop) {
    wb_sampler_inst *s = inst;
    free(s->samples);
    s->samples = malloc(count * sizeof(wb_sample));
    memcpy(s->samples, data, count * sizeof(wb_sample));
    s->sample_count = count;
    s->loop = loop;
}
void wb_sampler_note(void *inst, int note, int vel) {
    wb_sampler_inst *s = inst;
    if (vel == 0) {
        for (int i = 0; i < 16; i++)
            if (s->voices[i].active && s->voices[i].note == note) s->voices[i].active = 0;
        return;
    }
    int slot = -1;
    for (int i = 0; i < 16; i++) if (!s->voices[i].active) { slot = i; break; }
    if (slot < 0) slot = 0;
    wb_sampler_voice *v = &s->voices[slot];
    v->active = 1;
    v->note = note;
    v->pos = 0;
    v->vel = vel / 127.0f;
    v->rate = pow(2.0, (note - 60) / 12.0);  /* C4 = root */
}
void wb_sampler_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_sampler_inst *s = inst;
    for (uint32_t i = 0; i < n; i++) {
        float mix = 0;
        for (int v = 0; v < 16; v++) {
            wb_sampler_voice *vv = &s->voices[v];
            if (!vv->active || s->sample_count == 0) continue;
            double idx = vv->pos;
            if (idx >= s->sample_count) {
                if (s->loop) idx -= (double)s->sample_count;
                else { vv->active = 0; continue; }
            }
            uint32_t i0 = (uint32_t)idx;
            uint32_t i1 = i0 + 1 < s->sample_count ? i0 + 1 : i0;
            float frac = (float)(idx - i0);
            float samp = s->samples[i0] * (1.0f - frac) + s->samples[i1] * frac;
            mix += samp * vv->vel;
            vv->pos += vv->rate;
        }
        if (mix > 1.0f) mix = 1.0f; else if (mix < -1.0f) mix = -1.0f;
        L[i] = mix; R[i] = mix;
    }
}
