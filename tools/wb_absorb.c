/*
 * wb_absorb.c — Big Mac ABSORB: the full closed loop (E13 + B11)
 *
 *   demo.wav → analyze (F0/formants/quality) → fit 44 diameters →
 *   save voice-print.json → re-create → analyze the re-creation →
 *   comparison report (honest numbers).
 *
 * Usage: wb_absorb <demo.wav> <out.json> [out.wav]
 */
#include "wb_reader.h"
#include "wb_measure.h"
#include "wb_tract.h"
#include "wb_glottis.h"
#include "wb_wav.h"
#include "wb_print.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define SR 44100
#define BLOCK 1024
#define RENDER_SECS 0.5
#define N_SAMPLES ((int)(RENDER_SECS * SR))

static void render_diameters(const double *diams, int n, double f0, double *out, int nsamp) {
    wb_tract_t *tract = wb_tract_new(44);
    wb_tract_set_all_diameters(tract, diams, n);
    wb_glottis_t *g = wb_glottis_new();
    wb_glottis_set_frequency(g, f0);
    wb_glottis_set_intensity(g, 0.8);
    for (int j = 0; j < nsamp; j++) {
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
    wb_glottis_free(g);
    wb_tract_free(tract);
}

static int measure_f(const double *buf, double *F, int maxn) {
    wb_formant_measure_t m = wb_measure_formants(buf, (size_t)N_SAMPLES, SR);
    int n = m.n < maxn ? m.n : maxn;
    for (int i = 0; i < n; i++) F[i] = m.F[i];
    return n;
}

static double formant_error(const double *target, int nt, const double *cur, int nc) {
    double e = 0;
    for (int i = 0; i < nt; i++) {
        double c = i < nc ? cur[i] : 0.0;
        double d = (c - target[i]) / (target[i] > 0 ? target[i] : 1.0);
        e += d * d;
    }
    return sqrt(e / nt);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: wb_absorb <demo.wav> <out.json> [out.wav]\n");
        return 1;
    }
    const char *demo_path = argv[1];
    const char *json_path = argv[2];
    const char *out_path = argc > 3 ? argv[3] : "/tmp/recreated.wav";

    /* ---- 1. ANALYZE the demo ---- */
    wb_audio_t a;
    if (wb_audio_read(demo_path, &a) != 0) {
        fprintf(stderr, "cannot read %s\n", demo_path);
        return 1;
    }
    printf("=== ABSORB: %s (%d Hz, %.2f s) ===\n", demo_path, a.sample_rate, (double)a.n / a.sample_rate);

    wb_voiceprint_t vp;
    memset(&vp, 0, sizeof(vp));
    snprintf(vp.name, sizeof(vp.name), "%s", demo_path);

    /* use the middle half for analysis */
    size_t start = a.n / 4;
    size_t len = a.n / 2;
    if (len > a.n - start) len = a.n - start;
    vp.f0 = wb_measure_f0(a.data + start, len, a.sample_rate);
    vp.formants = wb_measure_formants(a.data + start, len, a.sample_rate);
    vp.quality = wb_measure_quality(a.data + start, len, a.sample_rate);

    printf("F0: mean=%.1f min=%.1f max=%.1f sd=%.1f voiced=%.0f%% vib=%.2fHz/%.1fc\n",
           vp.f0.f0_mean, vp.f0.f0_min, vp.f0.f0_max, vp.f0.f0_sd,
           vp.f0.voiced_fraction * 100, vp.f0.vibrato_rate, vp.f0.vibrato_depth);
    printf("formants:");
    for (int i = 0; i < vp.formants.n; i++) printf(" F%d=%.0f(BW%.0f)", i + 1, vp.formants.F[i], vp.formants.BW[i]);
    printf("\nquality: jitter=%.2f%% shimmer=%.2f%% HNR=%.1fdB CPP=%.2f H1-H2=%.1fdB tilt=%.2fdB/oct\n",
           vp.quality.jitter_pct, vp.quality.shimmer_pct, vp.quality.hnr_db,
           vp.quality.cpp, vp.quality.h1h2_db, vp.quality.tilt_db_per_oct);

    /* if the demo has no usable formants, still save what we have */
    if (vp.formants.n < 2) {
        printf("WARNING: demo yielded only %d formant(s); fitting needs >=2.\n", vp.formants.n);
    }

    /* ---- 2. FIT the 44 diameters to the demo's formants ---- */
    double target[4] = { vp.formants.F[0], vp.formants.F[1], vp.formants.F[2], vp.formants.F[3] };
    int ntarget = vp.formants.n > 3 ? 3 : vp.formants.n;
    double f0_render = vp.f0.f0_mean > 50 ? vp.f0.f0_mean : 140.0;
    vp.f0_render = f0_render;

    wb_tract_t *tract = wb_tract_new(44);
    const int N = wb_tract_n(tract);
    double *diams = malloc((size_t)N * sizeof(double));
    double *buf = malloc((size_t)N_SAMPLES * sizeof(double));
    if (!diams || !buf) { fprintf(stderr, "alloc fail\n"); return 1; }

    wb_tract_set_rest_diameter(tract, 12.9, 2.43);
    wb_tract_set_lips(tract, 0.9);
    wb_tract_get_all_diameters(tract, diams, N);

    int iters = 40;
    double eps = 0.04;
    double cur_F[4];
    int ncur;
    int converged = 0;

    printf("\nfitting %d diameters to demo formants...\n", N);
    for (int iter = 0; iter < iters; iter++) {
        wb_tract_set_all_diameters(tract, diams, N);
        render_diameters(diams, N, f0_render, buf, N_SAMPLES);
        ncur = measure_f(buf, cur_F, 3);
        double err = formant_error(target, ntarget, cur_F, ncur);
        if (iter % 8 == 0 || err < 0.04) {
            printf("  iter %2d: F=(%6.1f %6.1f %6.1f) err=%.4f\n",
                   iter, cur_F[0], cur_F[1], cur_F[2], err);
        }
        if (err < 0.04) { printf("  CONVERGED at iter %d\n", iter); converged = 1; break; }

        double J[4][64];
        memset(J, 0, sizeof(J));
        for (int j = 0; j < N; j++) {
            double save = diams[j];
            diams[j] = save + eps;
            render_diameters(diams, N, f0_render, buf, N_SAMPLES);
            double Fp[4]; int np = measure_f(buf, Fp, 3);
            diams[j] = save - eps;
            render_diameters(diams, N, f0_render, buf, N_SAMPLES);
            double Fm[4]; int nm = measure_f(buf, Fm, 3);
            diams[j] = save;
            for (int i = 0; i < 3; i++) {
                double fp = i < np ? Fp[i] : 0.0;
                double fm = i < nm ? Fm[i] : 0.0;
                J[i][j] = (fp - fm) / (2 * eps);
            }
        }

        double G[3][3], b[3];
        memset(G, 0, sizeof(G)); memset(b, 0, sizeof(b));
        for (int i = 0; i < 3; i++) {
            double resid = (i < ntarget ? target[i] : 0) - (i < ncur ? cur_F[i] : 0);
            b[i] = resid;
            for (int k = 0; k < 3; k++) {
                double s = 0;
                for (int j = 0; j < N; j++) s += J[i][j] * J[k][j];
                G[i][k] = s;
            }
        }
        double lambda = 0.3;
        for (int i = 0; i < 3; i++) G[i][i] += lambda;

        double M[3][4];
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) M[r][c] = G[r][c];
            M[r][3] = b[r];
        }
        for (int col = 0; col < 3; col++) {
            int piv = col;
            for (int r = col + 1; r < 3; r++)
                if (fabs(M[r][col]) > fabs(M[piv][col])) piv = r;
            if (fabs(M[piv][col]) < 1e-12) continue;
            for (int c = 0; c < 4; c++) { double t = M[col][c]; M[col][c] = M[piv][c]; M[piv][c] = t; }
            for (int r = 0; r < 3; r++) {
                if (r == col) continue;
                double f = M[r][col] / M[col][col];
                for (int c = col; c < 4; c++) M[r][c] -= f * M[col][c];
            }
        }
        double z[3] = { 0, 0, 0 };
        for (int r = 0; r < 3; r++)
            if (fabs(M[r][r]) > 1e-12) z[r] = M[r][3] / M[r][r];

        double *delta = malloc((size_t)N * sizeof(double));
        for (int j = 0; j < N; j++) {
            double s = 0;
            for (int i = 0; i < 3; i++) s += J[i][j] * z[i];
            delta[j] = s;
        }
        double scale = 1.0;
        for (int j = 0; j < N; j++) {
            double s = fabs(delta[j]) / 0.15;
            if (s > scale) scale = s;
        }
        for (int j = 0; j < N; j++) {
            diams[j] += delta[j] / (scale > 1 ? scale : 1.0);
            if (diams[j] < 0.05) diams[j] = 0.05;
            if (diams[j] > 3.0) diams[j] = 3.0;
        }
        free(delta);
    }
    (void)converged;
    wb_tract_free(tract);

    /* ---- 3. SAVE the voice-print ---- */
    memcpy(vp.diameters, diams, (size_t)N * sizeof(double));
    vp.n_diameters = N;
    if (wb_print_save(json_path, &vp) == 0) printf("voice-print saved: %s\n", json_path);

    /* ---- 4. RE-CREATE + COMPARE (E13) ---- */
    render_diameters(diams, N, f0_render, buf, N_SAMPLES);
    wb_wav_write(out_path, buf, (size_t)N_SAMPLES, SR);

    wb_f0_measure_t rf0 = wb_measure_f0(buf, (size_t)N_SAMPLES, SR);
    wb_formant_measure_t rf = wb_measure_formants(buf, (size_t)N_SAMPLES, SR);
    wb_quality_measure_t rq = wb_measure_quality(buf, (size_t)N_SAMPLES, SR);

    printf("\n=== COMPARISON (original vs re-creation) ===\n");
    printf("  F0:      %.1f Hz  vs  %.1f Hz   (err %.1f%%)\n",
           vp.f0.f0_mean, rf0.f0_mean,
           vp.f0.f0_mean > 0 ? fabs(rf0.f0_mean - vp.f0.f0_mean) / vp.f0.f0_mean * 100 : 0);
    printf("  Formants:");
    for (int i = 0; i < rf.n && i < 4; i++) {
        double o = i < vp.formants.n ? vp.formants.F[i] : 0;
        double pct = o > 0 ? fabs(rf.F[i] - o) / o * 100 : 0;
        printf(" F%d %.0f→%.0f (%.1f%%)", i + 1, o, rf.F[i], pct);
    }
    printf("\n  Jitter:  %.2f%% vs %.2f%%\n", vp.quality.jitter_pct, rq.jitter_pct);
    printf("  Shimmer: %.2f%% vs %.2f%%\n", vp.quality.shimmer_pct, rq.shimmer_pct);
    printf("  HNR:     %.1f dB vs %.1f dB\n", vp.quality.hnr_db, rq.hnr_db);
    printf("wrote re-creation: %s\n", out_path);

    free(diams); free(buf);
    wb_audio_free(&a);
    return 0;
}
