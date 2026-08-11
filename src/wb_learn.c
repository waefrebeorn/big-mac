/*
 * wb_learn.c — tabular Q-learner for the voice changer (strict C11)
 */
#include "wb_learn.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int wb_learn_f0_bucket(double f0) {
    if (f0 <= 40) return 0;
    if (f0 >= 500) return WB_LEARN_NF0 - 1;
    /* log-ish mapping: 40..500 over 8 buckets */
    double lg = (log(f0 / 40.0) / log(500.0 / 40.0)) * WB_LEARN_NF0;
    int b = (int)lg;
    if (b < 0) b = 0;
    if (b >= WB_LEARN_NF0) b = WB_LEARN_NF0 - 1;
    return b;
}

int wb_learn_form_bucket(double f1) {
    if (f1 <= 200) return 0;
    if (f1 >= 1000) return WB_LEARN_NFORM - 1;
    return (int)((f1 - 200) / 800.0 * WB_LEARN_NFORM);
}

int wb_learn_pick(const wb_learn_t *l, int s_f0, int s_f, double eps) {
    /* epsilon-greedy */
    double r = (double)rand() / (double)RAND_MAX;
    if (r < eps) return rand() % WB_LEARN_NACT;
    /* argmax Q */
    int best = 0;
    double bq = l->q[s_f0][s_f][0];
    for (int a = 1; a < WB_LEARN_NACT; a++) {
        if (l->q[s_f0][s_f][a] > bq) { bq = l->q[s_f0][s_f][a]; best = a; }
    }
    return best;
}

void wb_learn_update(wb_learn_t *l, int s_f0, int s_f, int act, double reward,
                     int s_f0_next, int s_f_next) {
    /* max Q over next state's actions */
    double maxq = l->q[s_f0_next][s_f_next][0];
    for (int a = 1; a < WB_LEARN_NACT; a++)
        if (l->q[s_f0_next][s_f_next][a] > maxq) maxq = l->q[s_f0_next][s_f_next][a];
    double target = reward + WB_LEARN_GAMMA * maxq;
    double cur = l->q[s_f0][s_f][act];
    l->q[s_f0][s_f][act] = cur + WB_LEARN_ALPHA * (target - cur);
    l->count[s_f0][s_f][act]++;
}

int wb_learn_save(const char *path, const wb_learn_t *l) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (int a = 0; a < WB_LEARN_NACT; a++) {
        for (int i = 0; i < WB_LEARN_NF0; i++) {
            for (int j = 0; j < WB_LEARN_NFORM; j++) {
                fprintf(f, "%d %d %d %.6f %d\n", i, j, a, l->q[i][j][a], l->count[i][j][a]);
            }
        }
    }
    fclose(f);
    return 0;
}

int wb_learn_load(const char *path, wb_learn_t *l) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int i, j, a, c;
    double q;
    while (fscanf(f, "%d %d %d %lf %d", &i, &j, &a, &q, &c) == 5) {
        if (i >= 0 && i < WB_LEARN_NF0 && j >= 0 && j < WB_LEARN_NFORM &&
            a >= 0 && a < WB_LEARN_NACT) {
            l->q[i][j][a] = q;
            l->count[i][j][a] = c;
        }
    }
    fclose(f);
    return 0;
}

void wb_learn_apply_tune(int act, double *ti, double *td, double *lips) {
    switch (act) {
    case 0: /* neutral */ break;
    case 1: /* brighter: tongue forward, narrower */ *ti -= 1.5; *td -= 0.2; break;
    case 2: /* darker: tongue back, wider */         *ti += 1.5; *td += 0.2; break;
    case 3: /* more open lips */                     *lips += 0.15; break;
    case 4: /* more closed lips (rounded) */         *lips -= 0.15; break;
    }
    if (*ti < 10) *ti = 10;
    if (*ti > 24) *ti = 24;
    if (*td < 0.4) *td = 0.4;
    if (*td > 2.5) *td = 2.5;
    if (*lips < 0.05) *lips = 0.05;
    if (*lips > 1.0) *lips = 1.0;
}
