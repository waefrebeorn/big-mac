/*
 * wb_vc.c — the WHIMSICAL VOICE CHANGER (backwards-RVC, one core)
 *
 * The magic trick: input voice in, cartoon voice out, in real time.
 *
 * Pipeline per chunk (RVC working BACKWARDS — synthesis-side first):
 *   1. ANALYSIS: YIN f0 + LPC formants + wuburvc voicing (per 40 ms)
 *   2. RETRIEVAL: (F1,F2) -> tongue/lips knobs via the prebuilt table
 *   3. LEARN: Q-table picks a tract tune for this voice region (and learns
 *      from the re-render: reward = formant match of what we produced)
 *   4. SYNTHESIS: render the chunk through the CHARACTER throat at the
 *      INPUT's f0 — your pitch, the character's timbre
 *
 * The character preset supplies: tract length, tenseness, vibrato,
 * jitter/shimmer (the "who"). The input supplies: f0 contour, formants,
 * voicing (the "what"). Combined per chunk = the street magic.
 *
 * Usage:
 *   wb_vc <in.wav> <character> <out.wav> [--learn qtable.txt] [--q qtable.txt]
 *   wb_vc --build-retrieve <retrieve.txt>   (precompute the F->knobs table)
 */
#include "wb_reader.h"
#include "wb_wav.h"
#include "wb_tract.h"
#include "wb_glottis.h"
#include "wb_measure.h"
#include "wb_dsp.h"
#include "wb_retrieve.h"
#include "wb_learn.h"
#include "wb_resample.h"
#include "wuburvc/wubu_consonant.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define SR 44100
#define BLOCK 1024
#define CHUNK_MS 40
#define CHUNK ((int)(SR * CHUNK_MS / 1000))

/* ---- character presets (same table as wb_toon, core params) ---- */
typedef struct { const char *name; double f0; double tenseness; double jitter;
                 double shimmer; double vib_depth; double vib_rate; int tract_n; } wb_char_t;

static const wb_char_t CHARS[] = {
    { "mickey",  320, 0.85, 0.004, 0.01, 0.004, 6.5, 36 },
    { "bugs",    175, 0.70, 0.006, 0.02, 0.003, 5.5, 44 },
    { "homer",   105, 0.45, 0.012, 0.04, 0.002, 4.5, 44 },
    { "donald",  250, 0.90, 0.008, 0.02, 0.005, 7.0, 36 },
    { "spongebob",300, 0.80, 0.006, 0.02, 0.004, 6.0, 36 },
    { "daffy",   210, 0.75, 0.007, 0.02, 0.004, 6.0, 44 },
    { "cartman", 330, 0.80, 0.005, 0.02, 0.003, 5.0, 28 },
    { "stewie",  240, 0.70, 0.005, 0.02, 0.003, 5.0, 36 },
    { "bart",    290, 0.65, 0.009, 0.03, 0.003, 5.5, 28 },
    { "scooby",  130, 0.55, 0.010, 0.03, 0.002, 4.5, 44 },
    { "betty",   380, 0.85, 0.004, 0.01, 0.005, 7.0, 36 },
    { "popeye",  120, 0.60, 0.014, 0.05, 0.002, 4.0, 44 },
    { "shaggy",  150, 0.50, 0.010, 0.03, 0.002, 4.5, 44 },
    { "yoda",    160, 0.55, 0.008, 0.03, 0.003, 5.0, 44 },
    { "bender",  140, 0.60, 0.010, 0.03, 0.002, 4.0, 44 },
};
#define N_CHARS (sizeof(CHARS)/sizeof(CHARS[0]))

static const wb_char_t *find_char(const char *name) {
    for (size_t i = 0; i < N_CHARS; i++)
        if (!strcmp(name, CHARS[i].name)) return &CHARS[i];
    return NULL;
}

/* ---- per-chunk analysis ---- */
typedef struct {
    double f0;          /* Hz, 0 = unvoiced */
    double f1, f2;      /* formants */
    int voiced;
} wb_chunk_analysis_t;

static wb_chunk_analysis_t analyze_chunk(const double *x, size_t n, int sr) {
    wb_chunk_analysis_t a;
    memset(&a, 0, sizeof(a));
    a.f0 = wb_yin_f0(x, n, sr);
    a.voiced = (a.f0 > 40 && a.f0 < 500);
    wb_formant_measure_t fm = wb_measure_formants(x, n, sr);
    a.f1 = fm.n > 0 ? fm.F[0] : 0;
    a.f2 = fm.n > 1 ? fm.F[1] : 0;
    return a;
}

/* ---- render one chunk through the character throat at input f0 ---- */
static void render_chunk(wb_tract_t *tract, wb_glottis_t *g,
                         double f0, double *out, int n) {
    if (f0 > 40 && f0 < 500) wb_glottis_set_frequency(g, f0);
    else wb_glottis_set_intensity(g, 0.0);   /* unvoiced: silence the glottis */
    for (int j = 0; j < n; j++) {
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
    if (!(f0 > 40 && f0 < 500)) wb_glottis_set_intensity(g, 0.8);
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--build-retrieve")) {
        if (argc < 3) { fprintf(stderr, "usage: wb_vc --build-retrieve <out.txt>\n"); return 1; }
        wb_retrieve_t t;
        printf("building retrieval table 9x9 (F1 250-900, F2 800-2800)...\n");
        clock_t t0 = clock();
        if (wb_retrieve_build(&t, 9, 9, 250, 900, 800, 2800) != 0) {
            fprintf(stderr, "build failed\n"); return 1;
        }
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        wb_retrieve_save(argv[2], &t);
        printf("built %dx%d table in %.0fs -> %s\n", t.n1, t.n2, secs, argv[2]);
        return 0;
    }

    if (argc < 4) {
        fprintf(stderr, "usage: wb_vc <in.wav> <character> <out.wav> [--learn q.txt] [--q q.txt]\n");
        return 1;
    }
    const char *in_path = argv[1];
    const wb_char_t *ch = find_char(argv[2]);
    if (!ch) { fprintf(stderr, "unknown character %s\n", argv[2]); return 1; }
    const char *out_path = argv[3];
    const char *q_path = NULL, *learn_path = NULL;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--q") && i + 1 < argc) q_path = argv[i+1];
        if (!strcmp(argv[i], "--learn") && i + 1 < argc) learn_path = argv[i+1];
    }

    /* retrieve table: load (or build once if missing) */
    wb_retrieve_t rtab;
    const char *rt_path = "/tmp/wb_retrieve.txt";
    if (wb_retrieve_load(rt_path, &rtab) != 0) {
        printf("building retrieval table (first run)...\n");
        if (wb_retrieve_build(&rtab, 9, 9, 250, 900, 800, 2800) != 0) { fprintf(stderr, "retrieve build failed\n"); return 1; }
        wb_retrieve_save(rt_path, &rtab);
    }

    /* Q table */
    wb_learn_t learn;
    memset(&learn, 0, sizeof(learn));
    if (q_path && wb_learn_load(q_path, &learn) != 0)
        printf("(no existing Q table, starting fresh)\n");

    wb_audio_t a0;
    if (wb_audio_read(in_path, &a0) != 0) { fprintf(stderr, "read fail\n"); return 1; }

    /* resample to 44.1k so chunks, durations and pitch are correct */
    wb_audio_t a = a0;
    if (a0.sample_rate != SR) {
        float *fin = malloc((size_t)a0.n * sizeof(float));
        size_t nout = (size_t)((double)a0.n * SR / a0.sample_rate) + 64;
        float *fout = malloc(nout * sizeof(float));
        if (!fin || !fout) { fprintf(stderr, "alloc fail\n"); return 1; }
        for (size_t i = 0; i < a0.n; i++) fin[i] = (float)a0.data[i];
        int got = wb_resample_sinc(fin, (int)a0.n, a0.sample_rate, SR, fout);
        if (got > 0) {
            a.data = malloc((size_t)got * sizeof(double));
            for (int i = 0; i < got; i++) a.data[i] = fout[i];
            a.n = (size_t)got;
            a.sample_rate = SR;
        }
        free(fin); free(fout);
    }
    printf("voice changer: %s -> %s (%.2fs @ %d Hz)\n", in_path, ch->name,
           (double)a.n / a.sample_rate, a.sample_rate);

    wb_tract_t *tract = wb_tract_new(ch->tract_n);
    wb_glottis_t *g = wb_glottis_new();
    wb_glottis_set_tenseness(g, ch->tenseness);
    wb_glottis_set_jitter(g, ch->jitter);
    wb_glottis_set_shimmer(g, ch->shimmer);
    wb_glottis_set_vibrato(g, ch->vib_depth, ch->vib_rate);
    wb_glottis_set_intensity(g, 0.8);

    int nchunks = (int)(a.n / CHUNK);
    double *out = calloc((size_t)a.n, sizeof(double));
    clock_t t0 = clock();

    double last_f0 = ch->f0;
    for (int c = 0; c < nchunks; c++) {
        size_t start = (size_t)c * CHUNK;
        wb_chunk_analysis_t an = analyze_chunk(a.data + start, CHUNK, a.sample_rate);

        /* f0: blend input pitch toward the character's base a little
         * (the "characterization" — you talk, the character sings) */
        double f0 = an.f0;
        if (an.voiced) {
            f0 = an.f0 * 0.7 + ch->f0 * 0.3;   /* 70% yours, 30% theirs */
            last_f0 = f0;
        } else {
            f0 = last_f0;  /* hold during unvoiced */
        }

        /* retrieval: formants -> knobs */
        double ti = 16.0, td = 0.9, lips = 0.7;
        if (an.f1 > 0) {
            wb_retrieve_lookup(&rtab, an.f1, an.f2 > 0 ? an.f2 : 1500.0, &ti, &td, &lips);
        }
        /* Q-learning: pick a tune for this voice region */
        int s_f0 = wb_learn_f0_bucket(an.voiced ? an.f0 : last_f0);
        int s_f = wb_learn_form_bucket(an.f1 > 0 ? an.f1 : 500);
        double eps = 0.2;
        int act = wb_learn_pick(&learn, s_f0, s_f, eps);
        wb_learn_apply_tune(act, &ti, &td, &lips);

        wb_tract_set_rest_diameter(tract, ti, td);
        wb_tract_set_lips(tract, lips);

        render_chunk(tract, g, f0, out + start, CHUNK);

        /* reward: how well did we match? measure our own output formants */
        wb_formant_measure_t fm = wb_measure_formants(out + start, CHUNK, SR);
        double our_f1 = fm.n > 0 ? fm.F[0] : 0;
        double reward = 0;
        if (an.f1 > 0 && our_f1 > 0) {
            reward = 1.0 - fabs(our_f1 - an.f1) / (an.f1 > 0 ? an.f1 : 1.0);
            if (reward < 0) reward = 0;
        }
        int s_f0n = wb_learn_f0_bucket(last_f0);
        int s_fn = wb_learn_form_bucket(our_f1 > 0 ? our_f1 : 500);
        wb_learn_update(&learn, s_f0, s_f, act, reward, s_f0n, s_fn);
    }
    clock_t t1 = clock();
    double proc = (double)(t1 - t0) / CLOCKS_PER_SEC;
    double audio_secs = (double)(nchunks * CHUNK) / SR;
    double rt = audio_secs > 0 ? proc / audio_secs : 0;

    wb_wav_write(out_path, out, (size_t)(nchunks * CHUNK), SR);
    printf("processed %.2fs audio in %.2fs -> realtime factor %.3fx %s\n",
           audio_secs, proc, rt, rt < 1.0 ? "REAL-TIME ✓" : "(needs speedup)");

    if (learn_path) {
        wb_learn_save(learn_path, &learn);
        /* summary: best action per region */
        int nactions[WB_LEARN_NACT] = {0};
        for (int i = 0; i < WB_LEARN_NF0; i++)
            for (int j = 0; j < WB_LEARN_NFORM; j++) {
                int best = 0;
                for (int a = 1; a < WB_LEARN_NACT; a++)
                    if (learn.q[i][j][a] > learn.q[i][j][best]) best = a;
                nactions[best]++;
            }
        printf("learned Q table saved: %s\n", learn_path);
        printf("  best-action distribution: neutral=%d brighter=%d darker=%d open=%d closed=%d\n",
               nactions[0], nactions[1], nactions[2], nactions[3], nactions[4]);
    }

    free(out);
    wb_glottis_free(g);
    wb_tract_free(tract);
    wb_audio_free(&a);
    return 0;
}
