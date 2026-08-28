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

/* ---- G2/G3 external render variants (defined in wb_fm_g2.c / wb_fm_g3.c) ---- */
extern void wb_fm_render_g2(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
extern void wb_fm_render_g3(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
extern void wb_fm_render_g3_simd(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* ---- Speed mode dispatch ---- */
static int fm_speed_mode = 0;  /* 0=auto, 1=scalar, 2=G2 SIMD, 3=G3 MT, 4=G3s MT+SIMD */

void wb_fm_set_speed_mode(int mode) {
    fm_speed_mode = mode;
}

/* wb_fm_render_fast: dispatches to the fastest available variant.
 * Called by the engine's u_fm_process (production render path).
 * wb_fm_render (below) remains the scalar bit-exact selftest gate. */
void wb_fm_render_fast(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    int mode = fm_speed_mode;
    if (mode == 0) {
        /* auto: G3s (MT+SIMD) is fastest on dual-core, G2 on single-core */
        #ifdef __APPLE__
        long ncpu = 2; (void)ncpu;
        /* runtime detection via sysctl would go here; default to G3s for dual-core i5 */
        #endif
        mode = 4; /* G3s MT+SIMD — best on this dual-core i5 */
    }
    switch (mode) {
        case 4: wb_fm_render_g3_simd(inst, L, R, n); break;
        case 3: wb_fm_render_g3(inst, L, R, n); break;
        case 2: wb_fm_render_g2(inst, L, R, n); break;
        default: wb_fm_render(inst, L, R, n); break;
    }
}

void wb_fm_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    fm_inst *f = inst;
    double sr = f->sr;
    /* FB1 (R076): hoist envelope exp constants — f->env_a/f->env_d/sr
     * are invariant across a render block. Saves 2*active_voices*frames
     * exp() calls per block. */
    double aD = exp(-1.0 / (f->env_d * sr));
    double aA = exp(-1.0 / (f->env_a * sr));
    /* FB2 (R076): precompute per-voice phase-step constants. v->freq,
     * f->ratio, sr are invariant across a block, so the phase/mphase
     * stepping deltas are computed once per voice, not per sample. */
    double phase_step[FM_VOICES], mphase_step[FM_VOICES];
    for (int k = 0; k < FM_VOICES; k++) {
        phase_step[k]   = TWO_PI * f->v[k].freq / sr;
        mphase_step[k]  = TWO_PI * f->v[k].freq * f->ratio / sr;
    }
    for (uint32_t i = 0; i < n; i++) {
        float sum = 0;
        for (int k = 0; k < FM_VOICES; k++) {
            fm_voice *v = &f->v[k];
            if (!v->active) continue;
            /* envelope (aD/aA already hoisted) */
            if (v->releasing) {
                v->env *= aD;
                if (v->env < 0.002) { v->active = 0; continue; }
            } else {
                if (v->env < 1.0) v->env = 1.0 - (1.0 - v->env) * aA;
            }
            double mod = f->index * v->env * sin(v->mphase);
            v->mphase += mphase_step[k];
            double s = sin(v->phase + mod);
            v->phase += phase_step[k];
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
static void u_fm_process(void *i, wb_sample *L, wb_sample *R, uint32_t n){ wb_fm_render_fast(i,L,R,n); }
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
