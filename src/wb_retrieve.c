/*
 * wb_retrieve.c — formant → articulation retrieval table (strict C11)
 *
 * Fits the 3 articulatory knobs (tongue index, tongue diameter, lips) to
 * each (F1, F2) target using the same damped least-squares loop as wb_fit,
 * but over just 3 parameters so it converges in a few iterations. The grid
 * is built offline; runtime lookup is bilinear interpolation.
 */
#include "wb_retrieve.h"
#include "wb_tract.h"
#include "wb_glottis.h"
#include "wb_measure.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define SR 44100
#define BLOCK 1024
#define RENDER_SECS 0.4
#define N_SAMPLES ((int)(RENDER_SECS * SR))

/* ---------- render a tract with given knobs ---------- */
static void render_knobs(double ti, double td, double lips, double f0,
                         double *out, int n) {
    wb_tract_t *tract = wb_tract_new(44);
    wb_tract_set_rest_diameter(tract, ti, td);
    wb_tract_set_lips(tract, lips);
    wb_glottis_t *g = wb_glottis_new();
    wb_glottis_set_frequency(g, f0);
    wb_glottis_set_intensity(g, 0.8);
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
    wb_glottis_free(g);
    wb_tract_free(tract);
}

static int measure_f(const double *buf, double *F, int maxn) {
    wb_formant_measure_t m = wb_measure_formants(buf, (size_t)N_SAMPLES, SR);
    int n = m.n < maxn ? m.n : maxn;
    for (int i = 0; i < n; i++) F[i] = m.F[i];
    return n;
}

int wb_retrieve_fit_pair(double f1, double f2,
                         double *ti, double *td, double *lips) {
    double target[3] = { f1, f2, 2500.0 };
    double params[3] = { 16.0, 0.9, 0.7 };
    double *buf = malloc((size_t)N_SAMPLES * sizeof(double));
    if (!buf) return -1;
    double eps = 0.05;
    double cur_F[3];
    int ncur;

    for (int iter = 0; iter < 20; iter++) {
        render_knobs(params[0], params[1], params[2], 140.0, buf, N_SAMPLES);
        ncur = measure_f(buf, cur_F, 3);
        /* error on F1 and F2 only */
        double e1 = (cur_F[0] - target[0]) / target[0];
        double e2 = ncur > 1 ? (cur_F[1] - target[1]) / target[1] : 1.0;
        double err = sqrt((e1*e1 + e2*e2) / 2.0);
        if (err < 0.04) break;

        /* Jacobian over 3 knobs (central differences) */
        double J[2][3];
        for (int k = 0; k < 3; k++) {
            double save = params[k];
            params[k] = save + eps;
            render_knobs(params[0], params[1], params[2], 140.0, buf, N_SAMPLES);
            double Fp[3]; int np = measure_f(buf, Fp, 3);
            params[k] = save - eps;
            render_knobs(params[0], params[1], params[2], 140.0, buf, N_SAMPLES);
            double Fm[3]; int nm = measure_f(buf, Fm, 3);
            params[k] = save;
            J[0][k] = ((np > 0 ? Fp[0] : 0) - (nm > 0 ? Fm[0] : 0)) / (2 * eps);
            J[1][k] = ((np > 1 ? Fp[1] : 0) - (nm > 1 ? Fm[1] : 0)) / (2 * eps);
        }

        /* damped least squares: (J^T J + lambda I) d = J^T r */
        double A[3][3], b[3];
        memset(A, 0, sizeof(A)); memset(b, 0, sizeof(b));
        double r[2] = { target[0] - cur_F[0], ncur > 1 ? target[1] - cur_F[1] : 0 };
        for (int i = 0; i < 2; i++) {
            for (int row = 0; row < 3; row++) {
                b[row] += J[i][row] * r[i];
                for (int col = 0; col < 3; col++) A[row][col] += J[i][row] * J[i][col];
            }
        }
        double lambda = 0.5;
        for (int i = 0; i < 3; i++) A[i][i] += lambda;
        double M[3][4];
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) M[r][c] = A[r][c];
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
        double d[3] = { 0, 0, 0 };
        for (int r = 0; r < 3; r++)
            if (fabs(M[r][r]) > 1e-12) d[r] = M[r][3] / M[r][r];
        /* clamp step */
        double scale = 1.0;
        double maxstep[3] = { 2.0, 0.8, 0.3 };
        for (int r = 0; r < 3; r++) {
            double s = fabs(d[r]) / maxstep[r];
            if (s > scale) scale = s;
        }
        for (int r = 0; r < 3; r++) params[r] += d[r] / (scale > 1 ? scale : 1.0);
        if (params[0] < 10) params[0] = 10;
        if (params[0] > 24) params[0] = 24;
        if (params[1] < 0.4) params[1] = 0.4;
        if (params[1] > 2.5) params[1] = 2.5;
        if (params[2] < 0.05) params[2] = 0.05;
        if (params[2] > 1.0) params[2] = 1.0;
    }

    free(buf);
    *ti = params[0];
    *td = params[1];
    *lips = params[2];
    return 0;
}

int wb_retrieve_build(wb_retrieve_t *t, int n1, int n2,
                      double f1_lo, double f1_hi, double f2_lo, double f2_hi) {
    if (n1 > WB_RETRIEVE_MAX_N || n2 > WB_RETRIEVE_MAX_N) return -1;
    memset(t, 0, sizeof(*t));
    t->n1 = n1; t->n2 = n2;
    t->f1_lo = f1_lo; t->f1_hi = f1_hi;
    t->f2_lo = f2_lo; t->f2_hi = f2_hi;
    for (int i = 0; i < n1; i++) {
        for (int j = 0; j < n2; j++) {
            double f1 = f1_lo + (f1_hi - f1_lo) * i / (n1 - 1);
            double f2 = f2_lo + (f2_hi - f2_lo) * j / (n2 - 1);
            if (wb_retrieve_fit_pair(f1, f2,
                                     &t->ti[i][j], &t->td[i][j], &t->lips[i][j]) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

int wb_retrieve_save(const char *path, const wb_retrieve_t *t) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%d %d\n", t->n1, t->n2);
    fprintf(f, "%.1f %.1f %.1f %.1f\n", t->f1_lo, t->f1_hi, t->f2_lo, t->f2_hi);
    for (int i = 0; i < t->n1; i++) {
        for (int j = 0; j < t->n2; j++) {
            fprintf(f, "%.3f %.3f %.3f\n", t->ti[i][j], t->td[i][j], t->lips[i][j]);
        }
    }
    fclose(f);
    return 0;
}

int wb_retrieve_load(const char *path, wb_retrieve_t *t) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (fscanf(f, "%d %d", &t->n1, &t->n2) != 2) { fclose(f); return -1; }
    if (fscanf(f, "%lf %lf %lf %lf", &t->f1_lo, &t->f1_hi, &t->f2_lo, &t->f2_hi) != 4) { fclose(f); return -1; }
    for (int i = 0; i < t->n1; i++)
        for (int j = 0; j < t->n2; j++)
            if (fscanf(f, "%lf %lf %lf", &t->ti[i][j], &t->td[i][j], &t->lips[i][j]) != 3) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int wb_retrieve_lookup(const wb_retrieve_t *t, double f1, double f2,
                       double *ti, double *td, double *lips) {
    if (f1 <= t->f1_lo) {
        if (f2 <= t->f2_lo) { *ti = t->ti[0][0]; *td = t->td[0][0]; *lips = t->lips[0][0]; return 0; }
        if (f2 >= t->f2_hi) { *ti = t->ti[0][t->n2-1]; *td = t->td[0][t->n2-1]; *lips = t->lips[0][t->n2-1]; return 0; }
        double u = (f2 - t->f2_lo) / (t->f2_hi - t->f2_lo) * (t->n2 - 1);
        int j = (int)u; if (j >= t->n2 - 1) j = t->n2 - 2;
        double w = u - j;
        *ti = t->ti[0][j] * (1 - w) + t->ti[0][j+1] * w;
        *td = t->td[0][j] * (1 - w) + t->td[0][j+1] * w;
        *lips = t->lips[0][j] * (1 - w) + t->lips[0][j+1] * w;
        return 0;
    }
    if (f1 >= t->f1_hi) {
        double u = (f2 - t->f2_lo) / (t->f2_hi - t->f2_lo) * (t->n2 - 1);
        int j = (int)u; if (j >= t->n2 - 1) j = t->n2 - 2;
        double w = u - j;
        *ti = t->ti[t->n1-1][j] * (1 - w) + t->ti[t->n1-1][j+1] * w;
        *td = t->td[t->n1-1][j] * (1 - w) + t->td[t->n1-1][j+1] * w;
        *lips = t->lips[t->n1-1][j] * (1 - w) + t->lips[t->n1-1][j+1] * w;
        return 0;
    }
    double u = (f1 - t->f1_lo) / (t->f1_hi - t->f1_lo) * (t->n1 - 1);
    double v = (f2 - t->f2_lo) / (t->f2_hi - t->f2_lo) * (t->n2 - 1);
    int i = (int)u; if (i >= t->n1 - 1) i = t->n1 - 2;
    int j = (int)v; if (j >= t->n2 - 1) j = t->n2 - 2;
    double wu = u - i, wv = v - j;
    *ti = t->ti[i][j] * (1-wu)*(1-wv) + t->ti[i+1][j] * wu*(1-wv)
        + t->ti[i][j+1] * (1-wu)*wv + t->ti[i+1][j+1] * wu*wv;
    *td = t->td[i][j] * (1-wu)*(1-wv) + t->td[i+1][j] * wu*(1-wv)
        + t->td[i][j+1] * (1-wu)*wv + t->td[i+1][j+1] * wu*wv;
    *lips = t->lips[i][j] * (1-wu)*(1-wv) + t->lips[i+1][j] * wu*(1-wv)
        + t->lips[i][j+1] * (1-wu)*wv + t->lips[i+1][j+1] * wu*wv;
    return 0;
}
