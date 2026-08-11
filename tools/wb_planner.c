/*
 * wb_planner.c — train the shallow MLP prosody planner (Lesson 1+6)
 *
 * The WordVoice bound-token idea, non-neural: a small MLP predicts
 * word-level acoustic attributes (dur_mult, energy, pitch, tone,
 * boundary) from word features. Training data is self-supervised:
 * we render sentences with our own TTS engine with KNOWN prosody plans
 * (the labels), then train the MLP to reproduce those plans from word
 * features — and optionally refine with measured reward.
 *
 * Usage:
 *   wb_planner train <out.mlp> [epochs] [seed]
 *   wb_planner predict <mlp> <word-features: 12 comma numbers>
 *   wb_planner selftest <mlp>
 */
#include "wb_mlp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------- synthetic training corpus (self-supervised) ----------------
 * Word features (12): [word_len, n_syllables, stress(0/1/2), position,
 *   is_first, is_last, has_vowel, punct_type(0 none 1 comma 2 period 3 ?),
 *   is_question_word, prev_stress, n_phones, word_freq_hint]
 * Targets (5): [dur_mult 0.5-2, energy 0-1, pitch -1..1, tone 0-6, boundary 0-4]
 */
#define NSAMPLES 4000

static void make_sample(double *f, double *t, int i, unsigned int *st) {
    unsigned int s = *st;
    *st = s * 1103515245u + 12345u;
    double r1 = ((s >> 16) & 0xFFFF) / 65535.0;
    s = *st; *st = s * 1103515245u + 12345u;
    double r2 = ((s >> 16) & 0xFFFF) / 65535.0;
    s = *st; *st = s * 1103515245u + 12345u;
    double r3 = ((s >> 16) & 0xFFFF) / 65535.0;
    s = *st; *st = s * 1103515245u + 12345u;
    double r4 = ((s >> 16) & 0xFFFF) / 65535.0;
    s = *st; *st = s * 1103515245u + 12345u;
    double r5 = ((s >> 16) & 0xFFFF) / 65535.0;
    (void)i;

    /* features */
    f[0] = 2 + r1 * 9;                          /* word length */
    f[1] = 1 + (int)(r2 * 3);                   /* syllables */
    f[2] = (int)(r3 * 3);                       /* stress 0..2 */
    f[3] = r4 * 10;                             /* position in phrase */
    f[4] = f[3] < 1.0 ? 1.0 : 0.0;              /* is_first */
    f[5] = f[3] > 9.0 ? 1.0 : 0.0;              /* is_last */
    f[6] = 1.0;                                 /* has_vowel */
    f[7] = (int)(r5 * 4);                       /* punct 0..3 */
    f[8] = f[7] == 3 ? 1.0 : 0.0;               /* is_question */
    f[9] = (int)(r2 * 3);                       /* prev_stress */
    f[10] = 3 + r1 * 8;                         /* n_phones */
    f[11] = 0.5 + r3;                           /* freq hint */

    /* targets — "acoustic thinking" rules:
     * t[0..2] regression: dur_mult, energy, pitch
     * t[3..9] one-hot tone (flat=0, rise=1, strong_rise=2, fall=3,
     *   strong_fall=4, peak=5, valley=6)
     * t[10..14] one-hot boundary (b0..b4) */
    t[0] = (0.8 + 0.3 * f[2] + 0.25 * f[5] + 0.15 * r1) / 2.0;  /* scale 0..1 */
    t[1] = 0.55 + 0.15 * f[2] + 0.15 * f[4] + 0.15 * r2;
    if (f[1] > 1.5) t[1] += 0.1;
    if (t[1] > 1.0) t[1] = 1.0;
    t[2] = (f[8] ? 0.6 * f[5] : -0.15) + 0.15 * f[2] + 0.2 * (r3 - 0.5);

    int tone;
    if (f[8] && f[5]) tone = 1;                /* rise */
    else if (f[5]) tone = 3;                   /* fall */
    else if (f[2] > 1.5 && f[3] > 2 && f[3] < 7) tone = 5; /* peak */
    else tone = 0;                              /* flat */
    for (int k = 3; k <= 9; k++) t[k] = (k - 3 == tone) ? 1.0 : 0.0;

    int bnd;
    if (f[7] == 1) bnd = 2;                    /* comma -> b2 */
    else if (f[7] == 2 || f[7] == 3) bnd = 3;  /* period/question -> b3 */
    else if (f[5]) bnd = 3;                    /* phrase end -> b3 */
    else bnd = 0;
    for (int k = 10; k <= 14; k++) t[k] = (k - 10 == bnd) ? 1.0 : 0.0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: wb_planner train <out.mlp> [epochs] [seed]\n");
        fprintf(stderr, "       wb_planner predict <mlp> <12 comma-separated features>\n");
        fprintf(stderr, "       wb_planner selftest <mlp>\n");
        return 1;
    }
    if (!strcmp(argv[1], "train")) {
        int epochs = argc > 3 ? atoi(argv[3]) : 500;
        unsigned int seed = argc > 4 ? (unsigned int)atoi(argv[4]) : 42;
        wb_mlp_t m;
        wb_mlp_init(&m, seed);
        static double feats[NSAMPLES][WB_MLP_IN];
        static double targs[NSAMPLES][WB_MLP_OUT];
        unsigned int st = seed;
        for (int i = 0; i < NSAMPLES; i++)
            make_sample(feats[i], targs[i], i, &st);
        printf("training %d samples, %d epochs...\n", NSAMPLES, epochs);
        for (int e = 0; e < epochs; e++) {
            double lr = 0.05 * (1.0 - (double)e / epochs);  /* anneal */
            double err = wb_mlp_train(&m, &feats[0][0], &targs[0][0], NSAMPLES, lr);
            if (e % 100 == 0 || e == epochs - 1)
                printf("  epoch %4d: mse %.6f\n", e, err);
        }
        wb_mlp_save(argv[2], &m);
        printf("saved planner -> %s\n", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "predict")) {
        wb_mlp_t m;
        if (wb_mlp_load(argv[2], &m) != 0) { fprintf(stderr, "load fail\n"); return 1; }
        double f[WB_MLP_IN], o[WB_MLP_OUT];
        char *tok = strtok(argv[3], ",");
        int i = 0;
        while (tok && i < WB_MLP_IN) { f[i++] = atof(tok); tok = strtok(NULL, ","); }
        while (i < WB_MLP_IN) f[i++] = 0;
        wb_mlp_forward(&m, f, o);
        /* argmax tone */
        int tone = 0; double tmax = o[3];
        for (int k = 4; k <= 9; k++) if (o[k] > tmax) { tmax = o[k]; tone = k - 3; }
        int bnd = 0; double bmax = o[10];
        for (int k = 11; k <= 14; k++) if (o[k] > bmax) { bmax = o[k]; bnd = k - 10; }
        static const char *TONE_NAMES[7] = { "flat","rise","strong_rise","fall","strong_fall","peak","valley" };
        printf("dur_mult=%.2f energy=%.2f pitch=%.2f tone=%s boundary=b%d\n",
               o[0] * 2.0, o[1], o[2], TONE_NAMES[tone], bnd);
        return 0;
    }
    if (!strcmp(argv[1], "selftest")) {
        wb_mlp_t m;
        if (wb_mlp_load(argv[2], &m) != 0) { fprintf(stderr, "load fail\n"); return 1; }
        static double feats[NSAMPLES][WB_MLP_IN];
        static double targs[NSAMPLES][WB_MLP_OUT];
        unsigned int st = 7;
        for (int i = 0; i < NSAMPLES; i++)
            make_sample(feats[i], targs[i], i, &st);
        double e = 0; int tone_ok = 0, bnd_ok = 0;
        for (int i = 0; i < NSAMPLES; i++) {
            double o[WB_MLP_OUT];
            wb_mlp_forward(&m, feats[i], o);
            for (int k = 0; k < 3; k++) { double d = o[k] - targs[i][k]; e += d * d; }
            int pt = 0; double tm = o[3];
            for (int k = 4; k <= 9; k++) if (o[k] > tm) { tm = o[k]; pt = k - 3; }
            int tt = 0; for (int k = 3; k <= 9; k++) if (targs[i][k] > 0.5) tt = k - 3;
            if (pt == tt) tone_ok++;
            int pb = 0; double bm = o[10];
            for (int k = 11; k <= 14; k++) if (o[k] > bm) { bm = o[k]; pb = k - 10; }
            int tb = 0; for (int k = 10; k <= 14; k++) if (targs[i][k] > 0.5) tb = k - 10;
            if (pb == tb) bnd_ok++;
        }
        printf("selftest: reg mse=%.5f  tone acc=%.1f%%  boundary acc=%.1f%%\n",
               e / (NSAMPLES * 3), 100.0 * tone_ok / NSAMPLES, 100.0 * bnd_ok / NSAMPLES);
        return 0;
    }
    fprintf(stderr, "unknown command\n");
    return 1;
}
