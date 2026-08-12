/* wb_fm.c — FM synthesizer unit (2-operator, polyphonic).
 * carrier pitch = note, modulator at ratio*freq, FM index modulaable.
 * Pure C11, all our math. Registered into the wb_unit registry.
 *
 * process() here is used when the FM synth is placed as a "voice unit"
 * (instrument). It is polyphonic: note() starts voices, render sums them.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "wb_unit.h"
#include "wb_internal.h"

#define FM_VOICES 16
#define TWO_PI 6.2831853071795864769

typedef struct {
    double phase;       /* carrier phase */
    double mphase;      /* modulator phase */
    double freq;
    int    active;
    int    note;        /* MIDI note, for note-on/off pairing */
    double env;         /* simple attack/decay envelope 0..1 */
    int    releasing;   /* in release: env decays to 0 */
    uint8_t vel;
} fm_voice;

typedef struct {
    uint32_t sr;
    double ratio;       /* modulator:carrier ratio */
    double index;       /* FM modulation index */
    double env_a;       /* attack time constant */
    double env_d;       /* decay time constant */
    fm_voice v[FM_VOICES];
} fm_inst;

void *wb_fm_create(uint32_t sr);
void  wb_fm_destroy(void *inst);
void  wb_fm_note(void *inst, int note, int vel);
void  wb_fm_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

void *wb_fm_create(uint32_t sr) {
    fm_inst *f = calloc(1, sizeof(*f));
    f->sr = sr;
    f->ratio = 2.0;      /* musical bright FM */
    f->index = 3.0;
    f->env_a = 0.002;    /* seconds */
    f->env_d = 0.30;
    return f;
}

void wb_fm_destroy(void *inst) { free(inst); }

void wb_fm_note(void *inst, int note, int vel) {
    fm_inst *f = inst;
    if (vel == 0) {
        /* note-off: mark the matching voice as releasing */
        for (int i = 0; i < FM_VOICES; i++)
            if (f->v[i].active && f->v[i].note == note) f->v[i].releasing = 1;
        return;
    }
    /* steal oldest inactive */
    int slot = -1;
    for (int i = 0; i < FM_VOICES; i++) if (!f->v[i].active) { slot = i; break; }
    if (slot < 0) { slot = 0; } /* steal voice 0 */
    fm_voice *v = &f->v[slot];
    v->active = 1;
    v->releasing = 0;
    v->note = note;
    v->vel = (uint8_t)vel;
    v->freq = wb_midi_note_to_freq(note);
    v->phase = 0; v->mphase = 0;
    v->env = 0;
}

void wb_fm_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    fm_inst *f = inst;
    double sr = f->sr;
    for (uint32_t i = 0; i < n; i++) {
        float sum = 0;
        for (int k = 0; k < FM_VOICES; k++) {
            fm_voice *v = &f->v[k];
            if (!v->active) continue;
            double aD = exp(-1.0 / (f->env_d * sr));
            /* envelope: attack, hold, or release */
            if (v->releasing) {
                v->env *= aD;
                if (v->env < 0.002) { v->active = 0; continue; }
            } else {
                double aA = exp(-1.0 / (f->env_a * sr));
                if (v->env < 1.0) v->env = 1.0 - (1.0 - v->env) * aA;
            }
            double mfreq = v->freq * f->ratio;
            double mod = f->index * v->env * sin(v->mphase);
            v->mphase += TWO_PI * mfreq / sr;
            double s = sin(v->phase + mod);
            v->phase += TWO_PI * v->freq / sr;
            sum += (float)(s * v->env * (v->vel / 127.0));
        }
        float out = sum * 0.35f; /* keep headroom for FM peaks */
        L[i] = out;
        R[i] = out;
    }
}

/* ---- wb_unit registration ----------------------------------------------- */
static void *u_fm_create(uint32_t sr){ return wb_fm_create(sr); }
static void u_fm_destroy(void *i){ wb_fm_destroy(i); }
static void u_fm_process(void *i, wb_sample *L, wb_sample *R, uint32_t n){ wb_fm_render(i,L,R,n); }
static void u_fm_note(void *i, int n, int v){ wb_fm_note(i,n,v); }
static const char *u_fm_id(void){ return "fm"; }
static int u_fm_has(const void *i, const char *name){
    (void)i;
    return !strcmp(name,"ratio") || !strcmp(name,"index");
}
static void u_fm_set(void *i, const char *name, float v){
    fm_inst *f = i;
    if (!strcmp(name,"ratio")) f->ratio = 0.5 + v*7.5;
    else if (!strcmp(name,"index")) f->index = v*8.0;
}
static float u_fm_get(const void *i, const char *name){
    const fm_inst *f = i;
    if (!strcmp(name,"ratio")) return (float)((f->ratio-0.5)/7.5);
    if (!strcmp(name,"index")) return f->index/8.0f;
    return 0;
}
static const wb_unit_vtable u_fm_vt = {
    u_fm_id, u_fm_create, u_fm_destroy, u_fm_process, u_fm_note,
    u_fm_has, u_fm_set, u_fm_get };
static const wb_unit u_fm_unit = { &u_fm_vt };

void wb_unit_ensure_fm(void) { static int d=0; if(!d){ wb_unit_register(&u_fm_unit); d=1; } }
