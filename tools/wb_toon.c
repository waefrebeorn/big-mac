/*
 * wb_toon.c — Big Mac CARTOON VOICE LAB
 *
 * Become the cartoon characters: each character is a voice-print preset
 * (f0, tenseness, jitter/shimmer, tract length, articulation bias)
 * rendered through the C11 engine. No samples — the physics IS the voice.
 *
 * Usage:
 *   wb_toon list                      # list characters
 *   wb_toon <name> <out.wav> [line]   # render a line as that character
 */
#include "wb_tract.h"
#include "wb_glottis.h"
#include "wb_wav.h"
#include "wb_measure.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define SR 44100
#define BLOCK 1024

/* ---------------- character presets ---------------- */

typedef struct {
    const char *name;
    double f0;            /* base pitch */
    double tenseness;     /* 0 breathy .. 1 pressed */
    double jitter;        /* vocal roughness */
    double shimmer;
    double vibrato_depth;
    double vibrato_rate;
    int tract_n;          /* 44 male / 36 female / 28 child */
    double tongue_index;  /* default articulation */
    double tongue_dia;
    double lips;
    const char *line;     /* signature line */
} wb_toon_t;

static const wb_toon_t TOONS[] = {
    { "mickey",  320, 0.85, 0.004, 0.01, 0.004, 6.5, 36, 13.5, 0.7, 0.80,
      "hee hee, oh boy, that was fun" },
    { "bugs",    175, 0.70, 0.006, 0.02, 0.003, 5.5, 44, 13.0, 0.9, 0.85,
      "eh, what's up doc" },
    { "homer",   105, 0.45, 0.012, 0.04, 0.002, 4.5, 44, 16.5, 1.3, 0.60,
      "d'oh, mmm donuts" },
    { "donald",  250, 0.90, 0.008, 0.02, 0.005, 7.0, 36, 14.0, 0.5, 0.35,
      "a-hyuck, oh boy oh boy" },
    { "spongebob", 300, 0.80, 0.006, 0.02, 0.004, 6.0, 36, 13.8, 0.8, 0.85,
      "i'm ready, i'm ready" },
    { "daffy",   210, 0.75, 0.007, 0.02, 0.004, 6.0, 44, 13.5, 0.6, 0.70,
      "you're despicable, thufferin' thuccotash" },
    { "cartman", 330, 0.80, 0.005, 0.02, 0.003, 5.0, 28, 13.8, 0.8, 0.80,
      "screw you guys, i'm going home" },
    { "stewie",  240, 0.70, 0.005, 0.02, 0.003, 5.0, 36, 14.0, 0.8, 0.75,
      "victory is mine" },
    { "bart",    290, 0.65, 0.009, 0.03, 0.003, 5.5, 28, 13.5, 0.8, 0.80,
      "eat my shorts" },
    { "scooby",  130, 0.55, 0.010, 0.03, 0.002, 4.5, 44, 15.5, 1.1, 0.70,
      "ruh roh, raggy" },
    { "betty",   380, 0.85, 0.004, 0.01, 0.005, 7.0, 36, 13.5, 0.7, 0.80,
      "boop oop a doop" },
    { "popeye",  120, 0.60, 0.014, 0.05, 0.002, 4.0, 44, 16.0, 1.2, 0.65,
      "i yam what i yam" },
    { "shaggy",  150, 0.50, 0.010, 0.03, 0.002, 4.5, 44, 15.0, 1.0, 0.75,
      "like, zoinks man" },
    { "yoda",    160, 0.55, 0.008, 0.03, 0.003, 5.0, 44, 15.0, 1.0, 0.70,
      "do or do not, there is no try" },
    { "bender",  140, 0.60, 0.010, 0.03, 0.002, 4.0, 44, 16.0, 1.1, 0.60,
      "bite my shiny metal ass" },
};

#define N_TOONS (sizeof(TOONS) / sizeof(TOONS[0]))

/* ---------------- tiny text -> phone mapping ---------------- */

/* very small dictionary of the words used in the signature lines */
static const char *dict(const char *w, char *out, size_t outsz) {
    struct { const char *w; const char *ph; } D[] = {
        { "i", "ay" }, { "im", "ay-m" }, { "oh", "ow" }, { "boy", "b-oy" },
        { "that", "dh-ae-t" }, { "was", "w-aa-z" }, { "fun", "f-ah-n" },
        { "whats", "w-ah-t-s" }, { "up", "ah-p" }, { "doc", "d-aa-k" },
        { "doh", "d-ow" }, { "mmm", "m-m-m" }, { "donuts", "d-ow-n-ah-t-s" },
        { "ahyuck", "aa-y-ah-k" }, { "ready", "r-eh-d-iy" },
        { "youre", "y-ao-r" }, { "despicable", "d-eh-s-p-ih-k-ah-b-ah-l" },
        { "thufferin", "th-ah-f-er-ih-n" }, { "thuccotash", "th-ah-k-ow-t-ae-sh" },
        { "screw", "s-k-r-uw" }, { "you", "y-uw" }, { "guys", "g-ay-z" },
        { "going", "g-ow-ih-ng" }, { "home", "h-ow-m" },
        { "victory", "v-ih-k-t-er-iy" }, { "is", "ih-z" }, { "mine", "m-ay-n" },
        { "eat", "iy-t" }, { "my", "m-ay" }, { "shorts", "sh-ao-r-t-s" },
        { "ruh", "r-ah" }, { "roh", "r-ow" }, { "raggy", "r-ae-g-iy" },
        { "boop", "b-uw-p" }, { "oop", "uw-p" }, { "a", "ah" }, { "doop", "d-uw-p" },
        { "yam", "y-ae-m" }, { "what", "w-ah-t" },
        { "like", "l-ay-k" }, { "zoinks", "z-oy-ih-ng-k-s" }, { "man", "m-ae-n" },
        { "do", "d-uw" }, { "or", "ao-r" }, { "not", "n-aa-t" }, { "there", "dh-eh-r" },
        { "no", "n-ow" }, { "try", "t-r-ay" },
        { "bite", "b-ay-t" }, { "shiny", "sh-ay-n-iy" }, { "metal", "m-eh-t-ah-l" },
        { "ass", "ae-s" }, { "he", "h-iy" }, { "hee", "h-iy" },
        { "oh", "ow" }, { "were", "w-er" },
    };
    for (size_t i = 0; i < sizeof(D) / sizeof(D[0]); i++) {
        if (strcmp(w, D[i].w) == 0) {
            snprintf(out, outsz, "%s", D[i].ph);
            return out;
        }
    }
    /* fallback: spell out */
    snprintf(out, outsz, "%s", w);
    return out;
}

/* ---------------- render a phone sequence ---------------- */

typedef struct { double ti, td, lips, velum; int voiced; double dur; } wb_gesture;

static double phone_ti(const char *p) {
    /* map vowel-ish phones to tongue positions */
    if (!strcmp(p, "aa")) return 20.0;
    if (!strcmp(p, "ae")) return 15.5;
    if (!strcmp(p, "ah")) return 16.0;
    if (!strcmp(p, "ao")) return 17.0;
    if (!strcmp(p, "ay")) return 15.5;
    if (!strcmp(p, "eh")) return 15.0;
    if (!strcmp(p, "er")) return 13.0;
    if (!strcmp(p, "ey")) return 14.5;
    if (!strcmp(p, "ih")) return 14.0;
    if (!strcmp(p, "iy")) return 13.5;
    if (!strcmp(p, "ow")) return 18.0;
    if (!strcmp(p, "oy")) return 17.0;
    if (!strcmp(p, "uh")) return 17.0;
    if (!strcmp(p, "uw")) return 18.0;
    return 13.5;
}

static double phone_td(const char *p) {
    if (!strcmp(p, "aa") || !strcmp(p, "ah") || !strcmp(p, "ow") || !strcmp(p, "uh") || !strcmp(p, "uw")) return 1.3;
    if (!strcmp(p, "iy") || !strcmp(p, "ih") || !strcmp(p, "er")) return 0.6;
    return 0.9;
}

static double phone_dur(const char *p) {
    if (!strcmp(p, " ") || !strcmp(p, "-")) return 0.04;
    /* vowels longer */
    if (strchr("aeiouy", p[0]) && strlen(p) <= 2) return 0.12;
    return 0.07;
}

static void render_line(const wb_toon_t *toon, const char *line,
                        double *out, int *n_out) {
    wb_tract_t *tract = wb_tract_new(toon->tract_n);
    wb_glottis_t *g = wb_glottis_new();
    wb_glottis_set_frequency(g, toon->f0);
    wb_glottis_set_tenseness(g, toon->tenseness);
    wb_glottis_set_jitter(g, toon->jitter);
    wb_glottis_set_shimmer(g, toon->shimmer);
    wb_glottis_set_vibrato(g, toon->vibrato_depth, toon->vibrato_rate);
    wb_glottis_set_intensity(g, 0.8);

    /* build phone list from line */
    char words[256][32];
    int nwords = 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", line);
    char *tok = strtok(tmp, " ,.!'");
    while (tok && nwords < 255) {
        snprintf(words[nwords], 32, "%s", tok);
        nwords++;
        tok = strtok(NULL, " ,.!'");
    }

    char phones[1024][8];
    int nphones = 0;
    for (int w = 0; w < nwords; w++) {
        char ph[128];
        dict(words[w], ph, sizeof(ph));
        char *ptok = strtok(ph, "-");
        while (ptok && nphones < 1023) {
            snprintf(phones[nphones], 8, "%s", ptok);
            nphones++;
            ptok = strtok(NULL, "-");
        }
        if (w + 1 < nwords) { snprintf(phones[nphones], 8, " "); nphones++; }
    }

    /* estimate total samples */
    double total = 0.2;  /* lead-in */
    for (int i = 0; i < nphones; i++) total += phone_dur(phones[i]);
    total += 0.3;  /* tail */
    int nsamp = (int)(total * SR);
    if (nsamp > SR * 10) nsamp = SR * 10;

    int pi = 0;
    double t = 0;
    for (int j = 0; j < nsamp; j++) {
        /* advance phones */
        while (pi < nphones && t >= 0) {
            /* crude: switch every phone_dur */
            (void)0;
            break;
        }
        double cur_t = (double)j / SR;
        if (pi < nphones) {
            /* figure out which phone we're in by accumulating durations */
            double acc = 0.2;
            int idx = 0;
            while (idx < nphones && acc + phone_dur(phones[idx]) <= cur_t) {
                acc += phone_dur(phones[idx]);
                idx++;
            }
            if (idx < nphones && strcmp(phones[idx], " ") && strcmp(phones[idx], "-")) {
                const char *p = phones[idx];
                wb_tract_set_rest_diameter(tract, phone_ti(p), phone_td(p));
                wb_tract_set_lips(tract, toon->lips);
                wb_glottis_set_tenseness(g, toon->tenseness);
            } else {
                wb_tract_set_rest_diameter(tract, toon->tongue_index, toon->tongue_dia);
                wb_tract_set_lips(tract, toon->lips);
            }
        }

        int m = j % BLOCK;
        double lam1 = (double)m / BLOCK, lam2 = (m + 0.5) / BLOCK;
        double noise = (double)((j * 2654435761u) >> 24) / 128.0 - 1.0;
        double gl = wb_glottis_run_step(g, lam1, noise * 0.3);
        double vocal = wb_tract_run_step(tract, gl, noise * 0.3, lam1)
                     + wb_tract_run_step(tract, gl, noise * 0.3, lam2);
        out[j] = vocal * 0.125;
        if (m == BLOCK - 1) {
            wb_glottis_finish_block(g, 1, (double)BLOCK / SR);
            wb_tract_finish_block(tract, (double)BLOCK / SR);
        }
    }
    *n_out = nsamp;
    wb_glottis_free(g);
    wb_tract_free(tract);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: wb_toon list | wb_toon <name> <out.wav> [line]\n");
        return 1;
    }
    if (!strcmp(argv[1], "list")) {
        printf("Big Mac cartoon voice lab — %zu characters\n", N_TOONS);
        for (size_t i = 0; i < N_TOONS; i++) {
            printf("  %-10s f0=%3.0f tense=%.2f jit=%.3f tract=%d  \"%s\"\n",
                   TOONS[i].name, TOONS[i].f0, TOONS[i].tenseness,
                   TOONS[i].jitter, TOONS[i].tract_n, TOONS[i].line);
        }
        return 0;
    }

    const wb_toon_t *toon = NULL;
    for (size_t i = 0; i < N_TOONS; i++) {
        if (!strcmp(argv[1], TOONS[i].name)) { toon = &TOONS[i]; break; }
    }
    if (!toon) { fprintf(stderr, "unknown character: %s\n", argv[1]); return 1; }
    if (argc < 3) { fprintf(stderr, "need output path\n"); return 1; }
    const char *line = argc > 3 ? argv[3] : toon->line;

    double *out = malloc((size_t)SR * 10 * sizeof(double));
    int n = 0;
    render_line(toon, line, out, &n);
    wb_wav_write(argv[2], out, (size_t)n, SR);

    /* verify with our own analyzer */
    wb_f0_measure_t f0m = wb_measure_f0(out, (size_t)n, SR);
    wb_quality_measure_t q = wb_measure_quality(out, (size_t)n, SR);
    printf("rendered %s as %s: %.2fs, measured f0=%.0fHz jitter=%.2f%% shimmer=%.2f%% hnr=%.1fdB\n",
           line, toon->name, (double)n / SR, f0m.f0_mean, q.jitter_pct,
           q.shimmer_pct, q.hnr_db);
    printf("wrote %s\n", argv[2]);

    free(out);
    return 0;
}
