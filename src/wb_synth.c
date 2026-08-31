/* wb_synth.c — subtractive polyphonic synthesizer instrument.
 * Implements the wbus plugin ABI. A handful of voices, each with osc(s),
 * filter, and ADSR. Rendered sample-by-sample into track buffers.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "wbus.h"
#include "wbus_plugin.h"
#include "wbus_dsp.h"

#define MAX_VOICES 16

typedef struct voice {
    int    active;
    int    note;
    int    vel;
    double freq;
    wb_osc osc1, osc2;
    wb_env env;
    wb_biquad filter;
} voice;

typedef struct wb_synth_inst {
    uint32_t sr;
    voice voices[MAX_VOICES];
    float   master_vol;
    float   filter_cutoff;
    float   filter_res;
    int     waveform;
    float   a, d, s, r;
} wb_synth_inst;

/* forward decls for the plugin vtable */
static const char *synth_id(const wb_plugin *p);
static const char *synth_name(const wb_plugin *p);
static uint32_t synth_param_count(const wb_plugin *p);
static void synth_param_info(const wb_plugin *p, uint32_t i, wb_param *out);
static void *synth_create(const wb_plugin *p, uint32_t sr);
static void synth_destroy(const wb_plugin *p, void *inst);
static float synth_get_param(const wb_plugin *p, void *inst, uint32_t id);
static void synth_set_param(const wb_plugin *p, void *inst, uint32_t id, float v);
static int synth_process(const wb_plugin *p, void *inst, wb_audio_block *b);

static const wb_plugin_vtable synth_vt = {
    synth_id, synth_name, synth_param_count, synth_param_info,
    synth_create, synth_destroy, synth_get_param, synth_set_param,
    synth_process
};
static const wb_plugin synth_plugin = { &synth_vt };

/* ---- registration ----------------------------------------------------- */
static int registered = 0;
void wb_synth_ensure_registered(void) {
    if (!registered) { wb_dsp_register(&synth_plugin); registered = 1; }
}

/* ---- helpers exposed to the engine (no plugin overhead on hot path) --- */
void *wb_synth_create(uint32_t sr) {
    wb_synth_inst *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->sr = sr;
    s->master_vol = 0.8f;
    s->filter_cutoff = 12000.0f;
    s->filter_res = 0.7f;
    s->waveform = WB_WAVE_SAW;
    s->a = 0.01f; s->d = 0.2f; s->s = 0.7f; s->r = 0.3f;
    for (int i = 0; i < MAX_VOICES; i++) {
        wb_env_init(&s->voices[i].env, (float)sr);
        wb_biquad_init(&s->voices[i].filter, (float)sr);
        wb_biquad_set(&s->voices[i].filter, 0, s->filter_cutoff, s->filter_res, 0);
    }
    return s;
}

void wb_synth_destroy(void *inst) { free(inst); }

/* Parameter setter for macro rack binding.
 * param: 0=filter_cutoff, 1=filter_res, 2=master_vol, 3=waveform */
void wb_synth_set(void *inst, int param, float v) {
    wb_synth_inst *s = inst;
    if (!s) return;
    switch (param) {
    case 0: s->filter_cutoff = v;
        for (int i = 0; i < MAX_VOICES; i++)
            wb_biquad_set(&s->voices[i].filter, 0, s->filter_cutoff, s->filter_res, 0);
        break;
    case 1: s->filter_res = v;
        for (int i = 0; i < MAX_VOICES; i++)
            wb_biquad_set(&s->voices[i].filter, 0, s->filter_cutoff, s->filter_res, 0);
        break;
    case 2: s->master_vol = v; break;
    case 3: s->waveform = (int)v; break;
    default: break;
    }
}

static double midi_to_freq(int note) {
    return 440.0 * pow(2.0, (note - 69) / 12.0);
}

void wb_synth_note(void *instp, int note, int vel) {
    wb_synth_inst *s = instp;
    if (!s) return;
    if (vel == 0) {
        /* note off: find matching note and release */
        for (int i = 0; i < MAX_VOICES; i++)
            if (s->voices[i].active && s->voices[i].note == note)
                wb_env_note_off(&s->voices[i].env);
        return;
    }
    /* find a free voice (or steal oldest) */
    int slot = -1, oldest = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!s->voices[i].active) { slot = i; break; }
        if (s->voices[i].note < s->voices[oldest].note) oldest = i;
    }
    if (slot < 0) slot = oldest;
    voice *v = &s->voices[slot];
    v->active = 1;
    v->note = note;
    v->vel = vel;
    v->freq = midi_to_freq(note);
    wb_osc_reset(&v->osc1);
    wb_osc_reset(&v->osc2);
    wb_env_note_on(&v->env, s->a, s->d, s->s, s->r);
    wb_biquad_set(&v->filter, 0, s->filter_cutoff, s->filter_res, 0);
}

void wb_synth_render_block(void *instp, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_synth_inst *s = instp;
    if (!s) return;
    float inc_scale = 1.0f / (float)s->sr;
    /* FC1 (R076): precompute per-voice oscillator phase-step.
     * vv->freq is invariant across a render block (set at note-on, only
     * changes via note-off + reassign), so the per-sample `inc = 2π*freq/sr`
     * multiply is hoisted out: phase_step[v] = 2π*freq*inc_scale, computed
     * once per active voice per block instead of MAX_VOICES*frames times. */
    float phase_step[MAX_VOICES];
    for (int v = 0; v < MAX_VOICES; v++) {
        voice *vv = &s->voices[v];
        phase_step[v] = vv->active ? (float)(2.0 * M_PI * vv->freq * inc_scale) : 0.0f;
    }

    for (uint32_t i = 0; i < n; i++) {
        float mixL = 0, mixR = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            voice *vv = &s->voices[v];
            if (!vv->active) continue;
            float inc = phase_step[v];
            float o1 = wb_osc_process(&vv->osc1, inc, s->waveform, 0.5f);
            float o2 = wb_osc_process(&vv->osc2, inc * 1.007f, WB_WAVE_SINE, 0.5f);
            float raw = o1 * 0.6f + o2 * 0.4f;
            raw = wb_biquad_process(&vv->filter, raw);
            float env = wb_env_process(&vv->env);
            float amp = env * (vv->vel / 127.0f) * s->master_vol;
            mixL += raw * amp;
            mixR += raw * amp;
            if (vv->env.stage == 0) vv->active = 0; /* voice finished */
        }
        /* hard clip guard */
        if (mixL > 1.0f) mixL = 1.0f; else if (mixL < -1.0f) mixL = -1.0f;
        if (mixR > 1.0f) mixR = 1.0f; else if (mixR < -1.0f) mixR = -1.0f;
        L[i] = mixL;
        R[i] = mixR;
    }
}

/* ---- plugin vtable impl ----------------------------------------------- */
static const char *synth_id(const wb_plugin *p) { (void)p; return "synth"; }
static const char *synth_name(const wb_plugin *p) { (void)p; return "Subtractive Synth"; }
static uint32_t synth_param_count(const wb_plugin *p) { (void)p; return 5; }
static void synth_param_info(const wb_plugin *p, uint32_t i, wb_param *out) {
    (void)p;
    static const char *names[] = {"volume","cutoff","res","attack","release"};
    if (i < 5) { out->id = i; snprintf(out->name,32,"%s",names[i]); out->min=0; out->max=1; out->default_value=0.5f; snprintf(out->units,16,""); }
}
static void *synth_create(const wb_plugin *p, uint32_t sr) { (void)p; return wb_synth_create(sr); }
static void synth_destroy(const wb_plugin *p, void *inst) { (void)p; wb_synth_destroy(inst); }
static float synth_get_param(const wb_plugin *p, void *inst, uint32_t id) {
    (void)p; wb_synth_inst *s = inst;
    switch (id) { case 0: return s->master_vol; case 1: return s->filter_cutoff/16000.0f; case 2: return s->filter_res; case 3: return s->a; case 4: return s->r; default: return 0; }
}
static void synth_set_param(const wb_plugin *p, void *inst, uint32_t id, float v) {
    (void)p; wb_synth_inst *s = inst;
    switch (id) { case 0: s->master_vol=v; break; case 1: s->filter_cutoff=v*16000.0f; break; case 2: s->filter_res=v; break; case 3: s->a=v; break; case 4: s->r=v; break; default: break; }
}
static int synth_process(const wb_plugin *p, void *inst, wb_audio_block *b) {
    (void)p;
    /* plugin-ABI path: render into the block's output buffers */
    wb_synth_inst *s = inst;
    uint32_t n = b->frames;
    float inc_scale = 1.0f / (float)s->sr;
    /* FC1 (R076): precompute per-voice oscillator phase-step (same as
     * wb_synth_render_block). vv->freq is invariant across the block. */
    float phase_step[MAX_VOICES];
    for (int v = 0; v < MAX_VOICES; v++) {
        voice *vv = &s->voices[v];
        phase_step[v] = vv->active ? (float)(2.0 * M_PI * vv->freq * inc_scale) : 0.0f;
    }
    for (uint32_t i = 0; i < n; i++) {
        float mixL = 0, mixR = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            voice *vv = &s->voices[v];
            if (!vv->active) continue;
            float inc = phase_step[v];
            float o1 = wb_osc_process(&vv->osc1, inc, s->waveform, 0.5f);
            float o2 = wb_osc_process(&vv->osc2, inc * 1.007f, WB_WAVE_SINE, 0.5f);
            float raw = o1 * 0.6f + o2 * 0.4f;
            raw = wb_biquad_process(&vv->filter, raw);
            float env = wb_env_process(&vv->env);
            float amp = env * (vv->vel / 127.0f) * s->master_vol;
            mixL += raw * amp;
            mixR += raw * amp;
            if (vv->env.stage == 0) vv->active = 0;
        }
        if (mixL > 1.0f) mixL = 1.0f; else if (mixL < -1.0f) mixL = -1.0f;
        if (mixR > 1.0f) mixR = 1.0f; else if (mixR < -1.0f) mixR = -1.0f;
        if (b->channels >= 1) b->outputs[0][i] = mixL;
        if (b->channels >= 2) b->outputs[1][i] = mixR;
    }
    return 0;
}
