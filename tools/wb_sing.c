/*
 * wb_sing.c — Big Mac SINGING MODE (C08: formant-tuned vocal line)
 *
 * Renders a melody through the vocal tract with singing characteristics:
 *   - glottal vibrato (rate/depth per character)
 *   - formant tuning: soprano-style F1 tracks F0 when F0 > F1 rest
 *     (the singer's trick — keeps vowels intelligible up high)
 *   - consonant articulation between notes (tongue/lip targets)
 *
 * Usage: wb_sing <out.wav> <f0_base> <tempo_bpm> <notes> [character]
 *   notes: comma-separated MIDI notes (e.g. 60,62,64,67,72)
 *   character: optional (mickey, homer, ...) for vibrato/tenseness flavor
 */
#include "wb_tract.h"
#include "wb_glottis.h"
#include "wb_wav.h"
#include "wb_aiff.h"
#include "wb_midi.h"
#include "wb_measure.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define SR 44100
#define BLOCK 1024

static double note_freq(int midi) {
    return 440.0 * pow(2.0, (midi - 69) / 12.0);
}

/* singing flavor presets */
typedef struct { const char *name; double vibrato_rate; double vibrato_depth; double tenseness; double jitter; } wb_sing_t;

static const wb_sing_t SINGERS[] = {
    { "default",  5.5, 0.006, 0.65, 0.004 },
    { "mickey",   6.5, 0.008, 0.85, 0.004 },
    { "homer",    4.5, 0.005, 0.45, 0.012 },
    { "betty",    7.0, 0.010, 0.85, 0.004 },
    { "spongebob",6.0, 0.008, 0.80, 0.006 },
    { "cartman",  5.0, 0.007, 0.80, 0.005 },
    { "yoda",     5.0, 0.006, 0.55, 0.008 },
};

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: wb_sing <out.wav> <f0_base> <bpm> <notes> [singer]\n");
        return 1;
    }
    const char *out_path = argv[1];
    double bpm = atof(argv[3]);

    /* parse notes */
    int notes[64];
    int nnotes = 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", argv[4]);
    char *tok = strtok(tmp, ",");
    while (tok && nnotes < 64) {
        notes[nnotes++] = atoi(tok);
        tok = strtok(NULL, ",");
    }
    if (nnotes == 0) { fprintf(stderr, "no notes\n"); return 1; }

    /* singer flavor */
    const wb_sing_t *s = &SINGERS[0];
    if (argc > 5) {
        for (size_t i = 0; i < sizeof(SINGERS)/sizeof(SINGERS[0]); i++) {
            if (!strcmp(argv[5], SINGERS[i].name)) { s = &SINGERS[i]; break; }
        }
    }

    wb_tract_t *tract = wb_tract_new(44);
    wb_glottis_t *g = wb_glottis_new();
    wb_glottis_set_tenseness(g, s->tenseness);
    wb_glottis_set_jitter(g, s->jitter);
    wb_glottis_set_shimmer(g, 0.01);
    wb_glottis_set_vibrato(g, s->vibrato_depth, s->vibrato_rate);
    wb_glottis_set_intensity(g, 0.85);

    /* note timing: quarter = 60/bpm sec; each note = 1 beat */
    double beat = 60.0 / bpm;
    double lead_in = 0.3;
    double total = lead_in + nnotes * beat + 0.5;
    int nsamp = (int)(total * SR);
    double *out = malloc((size_t)nsamp * sizeof(double));
    wb_midi_note_t mnotes[64];
    for (int i = 0; i < nnotes; i++) {
        mnotes[i].start_beats = i;
        mnotes[i].duration_beats = 0.85;
        mnotes[i].note = notes[i];
        mnotes[i].velocity = 95;
    }

    for (int j = 0; j < nsamp; j++) {
        double t = (double)j / SR;

        /* which note are we in? */
        int idx = (int)((t - lead_in) / beat);
        if (idx < 0) idx = 0;
        if (idx >= nnotes) idx = nnotes - 1;
        int midi = notes[idx];
        double f0 = note_freq(midi);

        /* formant tuning (C08): when F0 approaches F1, the singer raises
         * F1 to track F0 (soprano's trick). We emulate by moving the
         * tongue forward (raises F1) as f0 rises. */
        double f1_rest = 520.0;
        double f1_target = f1_rest;
        if (f0 > f1_rest * 0.8) f1_target = f1_rest * 0.8 + (f0 - f1_rest * 0.8) * 0.6;
        /* tongue index: higher f1 = more forward (lower index) */
        double ti = 20.0 - (f1_target - f1_rest) / f1_rest * 3.0;
        if (ti < 11.0) ti = 11.0;
        if (ti > 20.0) ti = 20.0;
        wb_tract_set_rest_diameter(tract, ti, 2.4);   /* open vowel tube (R013 fix) */
        wb_tract_set_lips(tract, 0.7);

        wb_glottis_set_frequency(g, f0);

        int m = j % BLOCK;
        double lam1 = (double)m / BLOCK, lam2 = (m + 0.5) / BLOCK;
        double noise = (double)((j * 2654435761u) >> 24) / 128.0 - 1.0;
        double gl = wb_glottis_run_step(g, lam1, noise * 0.2);
        double vocal = wb_tract_run_step(tract, gl, noise * 0.2, lam1)
                     + wb_tract_run_step(tract, gl, noise * 0.2, lam2);
        out[j] = vocal * 0.125;
        if (m == BLOCK - 1) {
            wb_glottis_finish_block(g, 1, (double)BLOCK / SR);
            wb_tract_finish_block(tract, (double)BLOCK / SR);
        }
    }

    wb_wav_write(out_path, out, (size_t)nsamp, SR);
    char aiff_path[512];
    snprintf(aiff_path, sizeof(aiff_path), "%s", out_path);
    char *dot = strrchr(aiff_path, '.');
    if (dot) strcpy(dot, ".aiff");
    wb_aiff_write(aiff_path, out, (size_t)nsamp, SR);

    char midi_path[512];
    snprintf(midi_path, sizeof(midi_path), "%s", out_path);
    char *mdot = strrchr(midi_path, '.');
    if (mdot) strcpy(mdot, ".mid");
    wb_midi_write(midi_path, mnotes, nnotes, (int)bpm, 480);

    /* verify: measure F0 of the final note region */
    double *tail = out + (size_t)((nnotes - 1) * beat * SR);
    size_t tlen = (size_t)(beat * 0.8 * SR);
    wb_f0_measure_t f0m = wb_measure_f0(tail, tlen, SR);
    double expect = note_freq(notes[nnotes - 1]);
    printf("sang %d notes as '%s' (bpm %.0f): final note %d = %.1f Hz, measured %.1f Hz (%.1f%% err)\n",
           nnotes, s->name, bpm, notes[nnotes-1], expect, f0m.f0_mean,
           fabs(f0m.f0_mean - expect) / expect * 100);
    printf("wrote %s (WAV+AIFF+MIDI)\n", out_path);

    free(out);
    wb_glottis_free(g);
    wb_tract_free(tract);
    return 0;
}
