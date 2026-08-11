/*
 * wb_mlp.c — shallow MLP prosody planner, trained in C11 (strict C11,
 * no third party). One hidden layer, tanh hidden, hybrid output:
 * 3 regression units (MSE loss) + 12 classification logits (softmax
 * cross-entropy: 7 tone classes + 5 boundary classes). Plain SGD.
 * ~300 params — trains in seconds on one core.
 */
#include "wb_mlp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double rnd(unsigned int *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return ((double)(*state & 0xFFFFFF) / 16777215.0) - 0.5;
}

void wb_mlp_init(wb_mlp_t *m, unsigned int seed) {
    unsigned int st = seed ? seed : 12345u;
    for (int i = 0; i < WB_MLP_IN; i++)
        for (int h = 0; h < WB_MLP_HID; h++) m->w1[i][h] = rnd(&st) * 0.5;
    for (int h = 0; h < WB_MLP_HID; h++) m->b1[h] = rnd(&st) * 0.1;
    for (int h = 0; h < WB_MLP_HID; h++)
        for (int o = 0; o < WB_MLP_OUT; o++) m->w2[h][o] = rnd(&st) * 0.5;
    for (int o = 0; o < WB_MLP_OUT; o++) m->b2[o] = rnd(&st) * 0.1;
}

void wb_mlp_forward(const wb_mlp_t *m, const double *features, double *out) {
    double h[WB_MLP_HID];
    for (int j = 0; j < WB_MLP_HID; j++) {
        double s = m->b1[j];
        for (int i = 0; i < WB_MLP_IN; i++) s += features[i] * m->w1[i][j];
        h[j] = tanh(s);
    }
    for (int k = 0; k < WB_MLP_OUT; k++) {
        double s = m->b2[k];
        for (int j = 0; j < WB_MLP_HID; j++) s += h[j] * m->w2[j][k];
        out[k] = s;
    }
}

double wb_mlp_train(wb_mlp_t *m, const double *feats, const double *targs,
                    int n, double lr) {
    double total = 0;
    for (int s = 0; s < n; s++) {
        const double *x = feats + (size_t)s * WB_MLP_IN;
        const double *y = targs + (size_t)s * WB_MLP_OUT;

        /* forward */
        double h[WB_MLP_HID], hz[WB_MLP_HID];
        for (int j = 0; j < WB_MLP_HID; j++) {
            double acc = m->b1[j];
            for (int i = 0; i < WB_MLP_IN; i++) acc += x[i] * m->w1[i][j];
            hz[j] = acc;
            h[j] = tanh(acc);
        }
        double o[WB_MLP_OUT];
        for (int k = 0; k < WB_MLP_OUT; k++) {
            double acc = m->b2[k];
            for (int j = 0; j < WB_MLP_HID; j++) acc += h[j] * m->w2[j][k];
            o[k] = acc;
        }

        /* loss: MSE on [0..2], softmax CE on tone [3..9] + boundary [10..14] */
        double loss = 0;
        for (int k = 0; k < 3; k++) { double d = o[k] - y[k]; loss += d * d; }
        double *dout = (double *)calloc(WB_MLP_OUT, sizeof(double));
        if (!dout) break;
        for (int k = 3; k < WB_MLP_OUT; k++) {
            double mxe = 0;
            for (int j = 3; j < WB_MLP_OUT; j++) (void)j;
            /* softmax over the group: tone group [3..9], boundary [10..14] */
            int gs, ge;
            if (k <= 9) { gs = 3; ge = 9; } else { gs = 10; ge = 14; }
            double mx = -1e30;
            for (int j = gs; j <= ge; j++) if (o[j] > mx) mx = o[j];
            double sum = 0;
            for (int j = gs; j <= ge; j++) sum += exp(o[j] - mx);
            double p = exp(o[k] - mx) / sum;
            loss -= y[k] * log(p > 1e-12 ? p : 1e-12);
            dout[k] = p - y[k];   /* softmax CE gradient */
            mxe = 0; (void)mxe;
        }
        total += loss;

        /* regression gradients */
        for (int k = 0; k < 3; k++) dout[k] = 2.0 * (o[k] - y[k]);

        /* hidden gradient */
        double dh[WB_MLP_HID];
        for (int j = 0; j < WB_MLP_HID; j++) {
            double acc = 0;
            for (int k = 0; k < WB_MLP_OUT; k++) acc += dout[k] * m->w2[j][k];
            dh[j] = acc * (1.0 - h[j] * h[j]);
        }

        /* update */
        for (int j = 0; j < WB_MLP_HID; j++)
            for (int k = 0; k < WB_MLP_OUT; k++)
                m->w2[j][k] -= lr * dout[k] * h[j];
        for (int k = 0; k < WB_MLP_OUT; k++) m->b2[k] -= lr * dout[k];
        for (int i = 0; i < WB_MLP_IN; i++)
            for (int j = 0; j < WB_MLP_HID; j++)
                m->w1[i][j] -= lr * dh[j] * x[i];
        for (int j = 0; j < WB_MLP_HID; j++) m->b1[j] -= lr * dh[j];
        free(dout);
    }
    return total / (n > 0 ? n : 1);
}

int wb_mlp_save(const char *path, const wb_mlp_t *m) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (int i = 0; i < WB_MLP_IN; i++)
        for (int h = 0; h < WB_MLP_HID; h++) fprintf(f, "%.8e\n", m->w1[i][h]);
    for (int h = 0; h < WB_MLP_HID; h++) fprintf(f, "%.8e\n", m->b1[h]);
    for (int h = 0; h < WB_MLP_HID; h++)
        for (int o = 0; o < WB_MLP_OUT; o++) fprintf(f, "%.8e\n", m->w2[h][o]);
    for (int o = 0; o < WB_MLP_OUT; o++) fprintf(f, "%.8e\n", m->b2[o]);
    fclose(f);
    return 0;
}

int wb_mlp_load(const char *path, wb_mlp_t *m) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    double v;
    for (int i = 0; i < WB_MLP_IN; i++)
        for (int h = 0; h < WB_MLP_HID; h++)
            if (fscanf(f, "%lf", &v) == 1) m->w1[i][h] = v; else { fclose(f); return -1; }
    for (int h = 0; h < WB_MLP_HID; h++)
        if (fscanf(f, "%lf", &v) == 1) m->b1[h] = v; else { fclose(f); return -1; }
    for (int h = 0; h < WB_MLP_HID; h++)
        for (int o = 0; o < WB_MLP_OUT; o++)
            if (fscanf(f, "%lf", &v) == 1) m->w2[h][o] = v; else { fclose(f); return -1; }
    for (int o = 0; o < WB_MLP_OUT; o++)
        if (fscanf(f, "%lf", &v) == 1) m->b2[o] = v; else { fclose(f); return -1; }
    fclose(f);
    return 0;
}
