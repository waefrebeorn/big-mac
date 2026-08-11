/*
 * wb_fit.c — Big Mac ABSORB CORE: formants → 44 KL diameters fitting loop
 *
 * Implements B01 (Kaburagi/Story in C11): given a target formant set
 * (measured from a demo), iterate ALL 44 tract diameters so the rendered
 * vowel's measured formants converge to the target.
 *
 * Method (Jacobian, damped least squares):
 *   1. Render current diameters, measure formants
 *   2. Perturb each diameter +eps, re-render, measure -> J[i][j] = dF_i/dD_j
 *   3. Solve damped normal equations for diameter deltas
 *   4. Apply (clamped), repeat until |err| < tol or max iters
 *
 * Usage: wb_fit <F1> <F2> <F3> [iters] [out.wav]
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
#define RENDER_SECS 0.5
#define N_SAMPLES ((int)(RENDER_SECS * SR))

/* ---------- render a tract to a buffer ---------- */
static void render_tract(const wb_tract_t *tract, double f0, double *out, int n) {
    wb_glottis_t *g = wb_glottis_new();
    wb_glottis_set_frequency(g, f0);
    wb_glottis_set_intensity(g, 0.8);
    for (int j = 0; j < n; j++) {
        int m = j % BLOCK;
        double lam1 = (double)m / BLOCK, lam2 = (m + 0.5) / BLOCK;
        double noise = (double)((j * 2654435761u) >> 24) / 128.0 - 1.0;
        double gl = wb_glottis_run_step(g, lam1, noise * 0.3);
        double vocal = wb_tract_run_step((wb_tract_t *)tract, gl, noise * 0.3, lam1)
                     + wb_tract_run_step((wb_tract_t *)tract, gl, noise * 0.3, lam2);
        out[j] = vocal * 0.125;
        if (m == BLOCK - 1) {
            wb_glottis_finish_block(g, 1, (double)BLOCK / SR);
            wb_tract_finish_block((wb_tract_t *)tract, (double)BLOCK / SR);
        }
    }
    wb_glottis_free(g);
}

/* ---------- measure formants of a rendered buffer ---------- */
static int measure_f(const double *buf, double *F, int maxn) {
    wb_formant_measure_t m = wb_measure_formants(buf, (size_t)N_SAMPLES, SR);
    int n = m.n < maxn ? m.n : maxn;
    for (int i = 0; i < n; i++) F[i] = m.F[i];
    return n;
}

/* ---------- normalized formant error ---------- */
static double formant_error(const double *target, int ntarget,
                            const double *cur, int ncur) {
    double e = 0;
    for (int i = 0; i < ntarget; i++) {
        double c = i < ncur ? cur[i] : 0.0;
        double d = (c - target[i]) / (target[i] > 0 ? target[i] : 1.0);
        e += d * d;
    }
    return sqrt(e / ntarget);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: wb_fit <F1> <F2> <F3> [iters] [out.wav]\n");
        return 1;
    }
    double target[3] = { atof(argv[1]), atof(argv[2]), atof(argv[3]) };
    int iters = argc > 4 ? atoi(argv[4]) : 60;
    const char *out_path = argc > 5 ? argv[5] : "/tmp/fit.wav";
    double f0 = 140.0;

    wb_tract_t *tract = wb_tract_new(44);
    const int N = wb_tract_n(tract);
    double *diams = malloc((size_t)N * sizeof(double));
    double *buf = malloc((size_t)N_SAMPLES * sizeof(double));
    if (!diams || !buf) { fprintf(stderr, "alloc fail\n"); return 1; }

    /* start from neutral rest diameters */
    wb_tract_set_rest_diameter(tract, 12.9, 2.43);
    wb_tract_set_lips(tract, 0.9);
    wb_tract_get_all_diameters(tract, diams, N);

    printf("fitting %d diameters to F1=%.0f F2=%.0f F3=%.0f (%d iters)\n",
           N, target[0], target[1], target[2], iters);

    double eps = 0.04;
    double cur_F[4];
    int ncur;
    double J[4][64];   /* nF (<=4) x 44 diameters */

    for (int iter = 0; iter < iters; iter++) {
        wb_tract_set_all_diameters(tract, diams, N);
        render_tract(tract, f0, buf, N_SAMPLES);
        ncur = measure_f(buf, cur_F, 3);
        double err = formant_error(target, 3, cur_F, ncur);
        if (iter % 5 == 0 || err < 0.04) {
            printf("iter %2d: F=(%6.1f %6.1f %6.1f) n=%d err=%.4f\n",
                   iter, cur_F[0], cur_F[1], cur_F[2], ncur, err);
        }
        if (err < 0.04) { printf("CONVERGED at iter %d\n", iter); break; }

        /* Jacobian via central differences */
        memset(J, 0, sizeof(J));
        for (int j = 0; j < N; j++) {
            double save = diams[j];

            diams[j] = save + eps;
            wb_tract_set_all_diameters(tract, diams, N);
            render_tract(tract, f0, buf, N_SAMPLES);
            double Fp[4]; int np = measure_f(buf, Fp, 3);

            diams[j] = save - eps;
            wb_tract_set_all_diameters(tract, diams, N);
            render_tract(tract, f0, buf, N_SAMPLES);
            double Fm[4]; int nm = measure_f(buf, Fm, 3);

            diams[j] = save;
            for (int i = 0; i < 3; i++) {
                double fp = i < np ? Fp[i] : 0.0;
                double fm = i < nm ? Fm[i] : 0.0;
                J[i][j] = (fp - fm) / (2 * eps);
            }
        }
        wb_tract_set_all_diameters(tract, diams, N);

        /* damped normal equations: (J^T J + lambda I) delta = J^T resid */
        /* nF x nF system; solve for delta in diameter space via
         * delta = J^T (J J^T + lambda I)^-1 resid  (underdetermined form) */
        double G[4][4], b[4];
        memset(G, 0, sizeof(G)); memset(b, 0, sizeof(b));
        for (int i = 0; i < 3; i++) {
            double resid = target[i] - (i < ncur ? cur_F[i] : 0.0);
            b[i] = resid;
            for (int k = 0; k < 3; k++) {
                double s = 0;
                for (int j = 0; j < N; j++) s += J[i][j] * J[k][j];
                G[i][k] = s;
            }
        }
        double lambda = 0.3;
        for (int i = 0; i < 3; i++) G[i][i] += lambda;

        /* solve 3x3 G x = b (Gaussian elimination) */
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

        /* delta_diameters = J^T z */
        double *delta = malloc((size_t)N * sizeof(double));
        for (int j = 0; j < N; j++) {
            double s = 0;
            for (int i = 0; i < 3; i++) s += J[i][j] * z[i];
            delta[j] = s;
        }
        /* clamp step: max 0.15 per diameter per iteration */
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

    /* final render + report */
    wb_tract_set_all_diameters(tract, diams, N);
    render_tract(tract, f0, buf, N_SAMPLES);
    double Ff[4]; int nf = measure_f(buf, Ff, 3);
    printf("\nFINAL formants: ");
    for (int i = 0; i < nf; i++) printf("F%d=%.0f ", i + 1, Ff[i]);
    printf("\ntarget:         ");
    for (int i = 0; i < 3; i++) printf("F%d=%.0f ", i + 1, target[i]);
    printf("\n");
    wb_wav_write(out_path, buf, (size_t)N_SAMPLES, SR);
    printf("wrote %s\n", out_path);

    free(diams); free(buf);
    wb_tract_free(tract);
    return 0;
}
