/* wb_drums.c — synthesis drum machine unit.
 * Models kick/snare/low/mid/hi toms/closed/open hihat/clap via sine/ noise
 * envelopes and a cheap LPF — no samples needed, pure C11. Registered into
 * the wb_unit registry. Driven by note() with a fixed map:
 *   pitch-map note -> sound:
 *     36 C1 kick    37 C#1 snare    38 D1  closed-hat
 *     39 D#1 clap   40 E1  open-hat  42 F#1 hi-mid tom
 *     44 G#1 low tom  46 A#1 hi tom  48 C2  crash(white noise)
 *
 * Drums are unpitched transients; we synthesize them into the track buffers.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "wb_unit.h"
#include "wb_internal.h"

#define TWO_PI 6.2831853071795864769
#define DRUM_VOICES 8

typedef struct { int note; int active; int kind; double env; double t; double f0; double phstep; } drum_voice;

typedef struct {
    uint32_t sr;
    drum_voice v[DRUM_VOICES];
} drum_inst;

void *wb_drum_create(uint32_t sr);
void  wb_drum_destroy(void *inst);
void  wb_drum_note(void *inst, int note, int vel);
void wb_drum_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void wb_drum_render_simd(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

void *wb_drum_create(uint32_t sr) {
    drum_inst *d = calloc(1, sizeof(*d));
    d->sr = sr;
    return d;
}
void wb_drum_destroy(void *inst) { free(inst); }

void wb_drum_note(void *inst, int note, int vel) {
    drum_inst *d = inst;
    if (vel == 0) return; /* no per-drum note-off for transient sounds */
    /* map note -> drum kind */
    int kind = -1;
    switch (note) {
        case 36: kind = 0; break; /* kick */
        case 37: kind = 1; break; /* snare */
        case 38: kind = 2; break; /* closed hat */
        case 39: kind = 3; break; /* clap */
        case 40: kind = 4; break; /* open hat */
        case 42: kind = 5; break; /* mid tom */
        case 44: kind = 6; break; /* low tom */
        case 46: kind = 7; break; /* hi tom */
        case 48: kind = 8; break; /* crash */
        default: kind = 0; break;
    }
    /* find a free voice */
    int slot = -1;
    for (int i = 0; i < DRUM_VOICES; i++) if (!d->v[i].active) { slot = i; break; }
    if (slot < 0) slot = 0;
    drum_voice *v = &d->v[slot];
    v->active = 1; v->note = note; v->kind = kind; v->t = 0; v->env = 1.0;
    v->f0 = (note > 36) ? wb_midi_note_to_freq(note) : 60.0;
    v->phstep = (note > 36) ? (TWO_PI * wb_midi_note_to_freq(note) / d->sr) : (TWO_PI * 60.0 / d->sr);
}

void wb_drum_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    drum_inst *d = inst;
    double inv_SR = 1.0 / (double)d->sr;  /* invariant across block */
    /* FB2 (R076): hoist per-kind envelope decay constants out of the sample loop.
     * dec is per-kind (invariant), sr is invariant, so exp(-1.0/(dec*sr)) is
     * computed once per kind per block instead of per active voice per sample. */
    double env_decay[9];  /* kind 0..8 */
    for (int k = 0; k < 9; k++) {
        double dec = 0.10;
        switch (k) {
            case 0: dec = 0.08; break;
            case 8: dec = 0.30; break;
            case 2: dec = 0.04; break;
            case 4: dec = 0.12; break;
            default: dec = 0.10; break;
        }
        env_decay[k] = exp(-1.0 / (dec * d->sr));
    }
    /* FB2: snare tone phase step (220 Hz constant) — invariant per block */
    double snare_phase_step = TWO_PI * 220.0 * inv_SR;
    for (uint32_t i = 0; i < n; i++) {
        float sL = L[i], sR = R[i];
        for (int k = 0; k < DRUM_VOICES; k++) {
            drum_voice *v = &d->v[k];
            if (!v->active) continue;
            v->t += 1.0;
            float out = 0;
            switch (v->kind) {
                case 0: { /* kick: decaying sine sweep 80->60Hz */
                    double f = 80.0 - (v->t * inv_SR / 0.05) * 20.0; if (f<40) f=40;
                    out = (float)(sin(v->t * TWO_PI * f * inv_SR) * v->env);
                    break;
                }
                case 1: { /* snare: noise + low tone */
                    float noise = ((float)rand()/(float)RAND_MAX) * 2.0f - 1.0f;
                    out = noise * (float)v->env * 0.5f;
                    out += (float)(sin(v->t * snare_phase_step) * v->env * 0.3);
                    break;
                }
                case 2: { /* closed hat: short noise LPF'd */
                    float noise = ((float)rand()/(float)RAND_MAX)*2.0f-1.0f;
                    out = noise * (float)v->env * 0.4f;
                    break;
                }
                case 3: { /* clap: noise burst with reverb-ish echo */
                    float noise = ((float)rand()/(float)RAND_MAX)*2.0f-1.0f;
                    out = noise * (float)v->env * 0.5f;
                    break;
                }
                case 4: { /* open hat: longer noise */
                    float noise = ((float)rand()/(float)RAND_MAX)*2.0f-1.0f;
                    out = noise * (float)v->env * 0.4f;
                    break;
                }
                case 8: { /* crash: white noise burst */
                    float noise = ((float)rand()/(float)RAND_MAX)*2.0f-1.0f;
                    out = noise * (float)v->env * 0.3f;
                    break;
                }
                default: { /* tom: decaying sine at note freq */
                    /* FC1: per-voice phase step = 2π*f0/sr, precomputed at note-on */
                    out = (float)(sin(v->t * v->phstep) * v->env * 0.5);
                }
            }
            /* per-kind decay — env_decay[] precomputed above */
            v->env *= env_decay[v->kind < 9 ? v->kind : 0];
            if (v->env < 0.001) { v->active = 0; continue; }
            sL += out; sR += out;
        }
        /* simple master baffle */
        L[i] = sL; R[i] = sR;
    }
}
/* ---- wb_unit registration ----------------------------------------------- */
static void *u_drum_create(uint32_t sr){ return wb_drum_create(sr); }
static void u_drum_destroy(void *i){ wb_drum_destroy(i); }
static void u_drum_process(void *i, wb_sample *L, wb_sample *R, uint32_t n){ wb_drum_render_simd(i,L,R,n); }
static void u_drum_note(void *i, int n, int v){ wb_drum_note(i,n,v); }
static const char *u_drum_id(void){ return "drum"; }
static const wb_unit_vtable u_drum_vt = {
    u_drum_id, u_drum_create, u_drum_destroy, u_drum_process, u_drum_note, 0,0,0 };
static const wb_unit u_drum_unit = { &u_drum_vt };

void wb_unit_ensure_drums(void) { static int d=0; if(!d){ wb_unit_register(&u_drum_unit); d=1; } }
