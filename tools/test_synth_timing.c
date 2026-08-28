/* R076 FC1 synth timing harness — before (inline per-sample inc divide) vs after (hoisted phase_step[]).
 * 8 active voices, 512 frames × 2000 blocks, 7 iterations each.
 * BEFORE: per-sample inc = 2π*freq/sr inside voice loop (old pattern).
 * AFTER:  per-voice phase_step[v] = 2π*freq/sr precomputed before sample loop (current source).
 * Measures wb_synth_render_block (the block render path, lines 120-150 of wb_synth.c). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "wb_unit.h"
#include "wbus.h"
#include "wb_internal.h"

#define TWO_PI 6.2831853071795864769
#define MAX_VOICES 16
#define POLYPHONIC 8

typedef struct { double freq; double t; int active; } synth_voice;

typedef struct {
    uint32_t sr;
    synth_voice voices[MAX_VOICES];
} synth_inst;

static double ts_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* ---- BEFORE: inline per-sample inc divide (old pattern) ---- */
static void before_synth_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    synth_inst *s = (synth_inst *)inst;
    for (uint32_t i = 0; i < n; i++) {
        float mixL = 0, mixR = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            synth_voice *vv = &s->voices[v];
            if (!vv->active) continue;
            vv->t += 1.0;
            /* OLD: per-sample inc = 2π*freq/sr — divide inside voice loop */
            float inc = (float)(TWO_PI * vv->freq / (double)s->sr);
            float raw = sin(vv->t * inc) * 0.5f; /* simplified: sine only, no filter/env */
            mixL += raw;
            mixR += raw;
        }
        if (mixL > 1.0f) mixL = 1.0f; else if (mixL < -1.0f) mixL = -1.0f;
        if (mixR > 1.0f) mixR = 1.0f; else if (mixR < -1.0f) mixR = -1.0f;
        L[i] = mixL; R[i] = mixR;
    }
}

/* ---- AFTER: hoisted phase_step[] (current wb_synth_render_block pattern) ---- */
static void after_synth_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    synth_inst *s = (synth_inst *)inst;
    float phase_step[MAX_VOICES];
    for (int v = 0; v < MAX_VOICES; v++) {
        synth_voice *vv = &s->voices[v];
        phase_step[v] = vv->active ? (float)(TWO_PI * vv->freq / (double)s->sr) : 0.0f;
    }
    for (uint32_t i = 0; i < n; i++) {
        float mixL = 0, mixR = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            synth_voice *vv = &s->voices[v];
            if (!vv->active) continue;
            vv->t += 1.0;
            float inc = phase_step[v]; /* hoisted — no per-sample divide */
            float raw = sin(vv->t * inc) * 0.5f;
            mixL += raw; mixR += raw;
        }
        if (mixL > 1.0f) mixL = 1.0f; else if (mixL < -1.0f) mixL = -1.0f;
        if (mixR > 1.0f) mixR = 1.0f; else if (mixR < -1.0f) mixR = -1.0f;
        L[i] = mixL; R[i] = mixR;
    }
}

static void fire_synth_voices(synth_inst *s) {
    for (int v = 0; v < POLYPHONIC; v++) {
        s->voices[v].freq = 220.0 * (v + 1); /* C3..ascending */
        s->voices[v].active = 1;
        s->voices[v].t = 0;
    }
}

int main(void) {
    uint32_t sr = 44100;
    uint32_t frames = 512;
    uint32_t blocks = 2000;

    synth_inst *s = calloc(1, sizeof(*s));
    if (!s) { fprintf(stderr, "alloc failed\n"); return 1; }
    s->sr = sr;
    fire_synth_voices(s);

    wb_sample *L = (wb_sample *)calloc(frames, sizeof(wb_sample));
    wb_sample *R = (wb_sample *)calloc(frames, sizeof(wb_sample));
    if (!L || !R) { fprintf(stderr, "buf alloc failed\n"); return 1; }

    /* ---- BEFORE (inline per-sample divide) ---- */
    {
        for (int v = 0; v < MAX_VOICES; v++) { s->voices[v].t = 0; s->voices[v].active = (v < POLYPHONIC); }
        int iter = 7;
        double best=1e100, worst=0, sum=0;
        for (int it=0; it<iter; it++) {
            double t0 = ts_ns();
            for (uint32_t b=0; b<blocks; b++) before_synth_render(s,L,R,frames);
            double t1 = ts_ns();
            double ns = t1 - t0;
            if (ns<best) best=ns;
            if (ns>worst) worst=ns;
            sum += ns;
        }
        double avg = sum/iter;
        printf("BEFORE synth render (inline per-sample inc, %d active voices, %u frames x %u blocks, %d iters)\n", POLYPHONIC, frames, blocks, iter);
        printf("  best   = %.0f ns   (%.0f ns/block)\n", best, best/blocks);
        printf("  worst  = %.0f ns   (%.0f ns/block)\n", worst, worst/blocks);
        printf("  avg    = %.0f ns   (%.0f ns/block)\n", avg, avg/blocks);
    }

    /* ---- AFTER (hoisted phase_step[]) ---- */
    {
        for (int v = 0; v < MAX_VOICES; v++) { s->voices[v].t = 0; s->voices[v].active = (v < POLYPHONIC); }
        int iter = 7;
        double best=1e100, worst=0, sum=0;
        for (int it=0; it<iter; it++) {
            double t0 = ts_ns();
            for (uint32_t b=0; b<blocks; b++) after_synth_render(s,L,R,frames);
            double t1 = ts_ns();
            double ns = t1 - t0;
            if (ns<best) best=ns;
            if (ns>worst) worst=ns;
            sum += ns;
        }
        double avg = sum/iter;
        printf("AFTER  synth render (hoisted phase_step[], %d active voices, %u frames x %u blocks, %d iters)\n", POLYPHONIC, frames, blocks, iter);
        printf("  best   = %.0f ns   (%.0f ns/block)\n", best, best/blocks);
        printf("  worst  = %.0f ns   (%.0f ns/block)\n", worst, worst/blocks);
        printf("  avg    = %.0f ns   (%.0f ns/block)\n", avg, avg/blocks);
    }

    free(L); free(R); free(s);
    return 0;
}
