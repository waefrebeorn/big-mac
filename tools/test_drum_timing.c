/* R076 FC1 drum timing harness — before (inline per-sample) vs after (hoisted).
 * 8 drum voices, 512 frames × 2000 blocks, 7 iterations each.
 * Before: per-sample sin(TWO_PI*f/sr) + exp(-1/(dec*sr)) + divide, no hoisting.
 * After:  phstep + snare_phase_step + env_decay[] + inv_SR hoisted (current source).
 * Runs standalone via `make test_drum_timing` — links against wb_drums.o + wb_midi_coremidi.o. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "wb_unit.h"
#include "wbus.h"
#include "wb_internal.h"

typedef struct { int note; int active; int kind; double env; double t; double f0; double phstep; } drum_voice;
typedef struct { uint32_t sr; drum_voice v[8]; } drum_inst;

#define TWO_PI 6.2831853071795864769

static double ts_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* ---- BEFORE: inline per-sample (no hoisting) ---- */
static void before_drum_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    drum_inst *d = (drum_inst *)inst;
    for (uint32_t i = 0; i < n; i++) {
        float sL = L[i], sR = R[i];
        for (int k = 0; k < 8; k++) {
            drum_voice *v = &d->v[k];
            if (!v->active) continue;
            v->t += 1.0;
            float out = 0;
            switch (v->kind) {
                case 0: { /* kick: sweep 80->60Hz, f not invariant */
                    double f = 80.0 - (v->t / (double)d->sr / 0.05) * 20.0; if (f<40) f=40;
                    out = (float)(sin(v->t * TWO_PI * f / (double)d->sr) * v->env);
                    break;
                }
                case 1: { /* snare: noise + 220Hz tone */
                    float noise = ((float)rand()/(float)RAND_MAX) * 2.0f - 1.0f;
                    out = noise * (float)v->env * 0.5f;
                    out += (float)(sin(v->t * TWO_PI * 220.0 / (double)d->sr) * v->env * 0.3f);
                    break;
                }
                case 2: case 3: case 4: case 8: { /* noise only */
                    float noise = ((float)rand()/(float)RAND_MAX)*2.0f-1.0f;
                    float gain = 0.4f;
                    if (v->kind == 3) gain = 0.5f;
                    if (v->kind == 4) gain = 0.4f;
                    if (v->kind == 8) gain = 0.3f;
                    out = noise * (float)v->env * gain;
                    break;
                }
                default: { /* tom */
                    out = (float)(sin(v->t * TWO_PI * v->f0 / (double)d->sr) * v->env * 0.5f);
                }
            }
            double dec = 0.10;
            switch (v->kind) {
                case 0: dec = 0.08; break;
                case 8: dec = 0.30; break;
                case 2: dec = 0.04; break;
                case 4: dec = 0.12; break;
                default: dec = 0.10; break;
            }
            v->env *= (float)exp(-1.0 / (dec * (double)d->sr));
            if (v->env < 0.001) { v->active = 0; continue; }
            sL += out; sR += out;
        }
        L[i] = sL; R[i] = sR;
    }
}

int main(void) {
    srand(12345);
    uint32_t sr = 44100;
    uint32_t frames = 512;
    uint32_t blocks = 2000;

    void *d_void = wb_drum_create(sr);
    if (!d_void) { fprintf(stderr, "drum create failed\n"); return 1; }
    drum_inst *d = (drum_inst *)d_void;

    int notes[] = {36, 37, 38, 39, 40, 42, 44, 46};
    for (int i = 0; i < 8; i++) wb_drum_note(d, notes[i], 100);

    wb_sample *L = (wb_sample *)calloc(frames, sizeof(wb_sample));
    wb_sample *R = (wb_sample *)calloc(frames, sizeof(wb_sample));
    if (!L || !R) { fprintf(stderr, "buf alloc failed\n"); return 1; }

    /* ---- BEFORE ---- */
    {
        for (int k = 0; k < 8; k++) { d->v[k].env=1.0; d->v[k].active=1; d->v[k].t=0; }
        int iter = 7;
        double best=1e100, worst=0, sum=0;
        for (int it=0; it<iter; it++) {
            double t0 = ts_ns();
            for (uint32_t b=0; b<blocks; b++) before_drum_render(d,L,R,frames);
            double t1 = ts_ns();
            double ns = t1 - t0;
            if (ns<best) best=ns;
            if (ns>worst) worst=ns;
            sum += ns;
        }
        double avg = sum/iter;
        printf("BEFORE drum render (inline per-sample, 8 voices, %u frames x %u blocks, %d iters)\n", frames, blocks, iter);
        printf("  best   = %.0f ns   (%.0f ns/block)\n", best, best/blocks);
        printf("  worst  = %.0f ns   (%.0f ns/block)\n", worst, worst/blocks);
        printf("  avg    = %.0f ns   (%.0f ns/block)\n", avg, avg/blocks);
    }

    /* ---- AFTER (hoisted) ---- */
    {
        for (int k = 0; k < 8; k++) { d->v[k].env=1.0; d->v[k].active=1; d->v[k].t=0; }
        int iter = 7;
        double best=1e100, worst=0, sum=0;
        for (int it=0; it<iter; it++) {
            double t0 = ts_ns();
            for (uint32_t b=0; b<blocks; b++) wb_drum_render(d,L,R,frames);
            double t1 = ts_ns();
            double ns = t1 - t0;
            if (ns<best) best=ns;
            if (ns>worst) worst=ns;
            sum += ns;
        }
        double avg = sum/iter;
        printf("AFTER  drum render (hoisted phstep+env_decay+inv_SR, 8 voices, %u frames x %u blocks, %d iters)\n", frames, blocks, iter);
        printf("  best   = %.0f ns   (%.0f ns/block)\n", best, best/blocks);
        printf("  worst  = %.0f ns   (%.0f ns/block)\n", worst, worst/blocks);
        printf("  avg    = %.0f ns   (%.0f ns/block)\n", avg, avg/blocks);
    }

    free(L); free(R);
    wb_drum_destroy(d);
    return 0;
}
