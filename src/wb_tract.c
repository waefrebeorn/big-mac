/*
 * wb_tract.c — vocal tract waveguide implementation (strict C11)
 */
#include "wb_tract.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

struct wb_tract {
    int n;
    int blade_start, tip_start, lip_start;
    int nose_length, nose_start;

    double *R;   /* right-going wave */
    double *L;   /* left-going wave  */
    double *reflection;
    double *new_reflection;
    double *junction_R;
    double *junction_L;
    double *diameter;
    double *rest_diameter;
    double *target_diameter;
    double *A;

    double glottal_reflection;
    double lip_reflection;
    int last_obstruction;
    double movement_speed;

    /* R013 mouth controls */
    double lip_rounding;   /* 0..1 lip protrusion/rounding (pursed terminal tube) */
    double noise_index;    /* frication source section; <0 = default (tip_start) */

    double reflection_left, reflection_right, reflection_nose;
    double new_reflection_left, new_reflection_right, new_reflection_nose;

    double *nose_R, *nose_L;
    double *nose_junction_R, *nose_junction_L;
    double *nose_reflection;
    double *nose_diameter;
    double *nose_A;
    double velum_target;

    double lip_output, nose_output;

    /* transient bursts (stop consonants) */
    struct { int position; double time_alive, life_time, strength, exponent; } *transients;
    size_t n_transients, cap_transients;
};

static double clampd(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static double move_towards(double cur, double tgt, double up, double down) {
    if (cur < tgt) { double r = cur + up; return r < tgt ? r : tgt; }
    double r = cur - down; return r > tgt ? r : tgt;
}

wb_tract_t *wb_tract_new(int n) {
    wb_tract_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->n = n;
    t->blade_start = (int)(10 * n / 44.0);
    t->tip_start   = (int)(32 * n / 44.0);
    t->lip_start   = (int)(39 * n / 44.0);
    t->nose_length = (int)(28 * n / 44.0);
    t->nose_start  = n - t->nose_length + 1;

    t->R = calloc((size_t)n, sizeof(double));
    t->L = calloc((size_t)n, sizeof(double));
    t->reflection = calloc((size_t)(n + 1), sizeof(double));
    t->new_reflection = calloc((size_t)(n + 1), sizeof(double));
    t->junction_R = calloc((size_t)(n + 1), sizeof(double));
    t->junction_L = calloc((size_t)(n + 1), sizeof(double));
    t->diameter = calloc((size_t)n, sizeof(double));
    t->rest_diameter = calloc((size_t)n, sizeof(double));
    t->target_diameter = calloc((size_t)n, sizeof(double));
    t->A = calloc((size_t)n, sizeof(double));

    t->nose_R = calloc((size_t)t->nose_length, sizeof(double));
    t->nose_L = calloc((size_t)t->nose_length, sizeof(double));
    t->nose_junction_R = calloc((size_t)(t->nose_length + 1), sizeof(double));
    t->nose_junction_L = calloc((size_t)(t->nose_length + 1), sizeof(double));
    t->nose_reflection = calloc((size_t)(t->nose_length + 1), sizeof(double));
    t->nose_diameter = calloc((size_t)t->nose_length, sizeof(double));
    t->nose_A = calloc((size_t)t->nose_length, sizeof(double));

    t->glottal_reflection = 0.75;
    t->lip_reflection = -0.85;
    t->last_obstruction = -1;
    t->movement_speed = 15.0;
    t->velum_target = 0.01;
    t->lip_rounding = 0.0;
    t->noise_index = -1.0;

    /* rest diameters */
    for (int i = 0; i < n; i++) {
        double d;
        if (i < 7 * n / 44.0 - 0.5) d = 0.6;
        else if (i < 12 * n / 44.0) d = 1.1;
        else d = 1.5;
        t->diameter[i] = t->rest_diameter[i] = t->target_diameter[i] = d;
    }
    /* nasal cavity profile */
    for (int i = 0; i < t->nose_length; i++) {
        double d = 2.0 * (i / (double)t->nose_length);
        double dia = d < 1 ? 0.4 + 1.6 * d : 0.5 + 1.5 * (2 - d);
        t->nose_diameter[i] = dia < 1.9 ? dia : 1.9;
    }
    t->nose_diameter[0] = t->velum_target;
    for (int i = 0; i < t->nose_length; i++) {
        t->nose_A[i] = t->nose_diameter[i] * t->nose_diameter[i];
    }

    /* initial reflections */
    for (int i = 0; i < n; i++) t->A[i] = t->diameter[i] * t->diameter[i];
    for (int i = 1; i < n; i++) {
        t->new_reflection[i] = (t->A[i-1] - t->A[i]) / (t->A[i-1] + t->A[i]);
    }
    {
        double s = t->A[t->nose_start] + t->A[t->nose_start + 1] + t->nose_A[0];
        t->new_reflection_left  = (2 * t->A[t->nose_start]     - s) / s;
        t->new_reflection_right = (2 * t->A[t->nose_start + 1] - s) / s;
        t->new_reflection_nose  = (2 * t->nose_A[0]            - s) / s;
        t->reflection_left = t->new_reflection_left;
        t->reflection_right = t->new_reflection_right;
        t->reflection_nose = t->new_reflection_nose;
    }
    for (int i = 1; i < t->nose_length; i++) {
        t->nose_reflection[i] = (t->nose_A[i-1] - t->nose_A[i]) / (t->nose_A[i-1] + t->nose_A[i]);
    }
    return t;
}

void wb_tract_free(wb_tract_t *t) {
    if (!t) return;
    free(t->R); free(t->L); free(t->reflection); free(t->new_reflection);
    free(t->junction_R); free(t->junction_L); free(t->diameter);
    free(t->rest_diameter); free(t->target_diameter); free(t->A);
    free(t->nose_R); free(t->nose_L); free(t->nose_junction_R); free(t->nose_junction_L);
    free(t->nose_reflection); free(t->nose_diameter); free(t->nose_A);
    free(t->transients);
    free(t);
}

void wb_tract_set_rest_diameter(wb_tract_t *t, double tongue_index, double tongue_diameter) {
    for (int i = t->blade_start; i < t->lip_start; i++) {
        double tt = 1.1 * M_PI * (tongue_index - i) / (double)(t->tip_start - t->blade_start);
        double fixed = 2.0 + (tongue_diameter - 2.0) / 1.5;
        double curve = (1.5 - fixed + 1.7) * cos(tt);
        if (i == t->blade_start - 2 || i == t->lip_start - 1) curve *= 0.8;
        if (i == t->blade_start     || i == t->lip_start - 2) curve *= 0.94;
        t->rest_diameter[i] = 1.5 - curve;
    }
}

void wb_tract_set_lips(wb_tract_t *t, double aperture) {
    /* The terminal tube (lip region) is narrowed to the aperture. With lip
     * rounding > 0 we additionally purse it into a horn — the narrowing is
     * strongest at the very lip and tapers backward, modelling protrusion
     * (a longer, narrower lip tube). This is what pushes F2/F3 down for
     * rounded vowels /u o/ and labialized /w/ (rounding acoustics). */
    double r = t->lip_rounding;
    int lo = t->lip_start - 2;
    int span = t->n - lo;                 /* terminal tube length */
    for (int i = t->lip_start - 2; i < t->n; i++) {
        double v = aperture * 1.9;
        if (v > t->rest_diameter[i]) v = t->rest_diameter[i];
        if (r > 0.0 && span > 0) {
            double fwd = (double)(i - lo) / (double)span;   /* 0 at tube start, 1 at lip */
            double narrow = 1.0 - 0.6 * r * (0.35 + 0.65 * fwd);
            v *= narrow;
        }
        t->target_diameter[i] = v;
    }
}

void wb_tract_set_lip_rounding(wb_tract_t *t, double amount) {
    t->lip_rounding = clampd(amount, 0.0, 1.0);
}

void wb_tract_set_noise_pos(wb_tract_t *t, double index) {
    t->noise_index = index;
}

void wb_tract_set_teeth(wb_tract_t *t, double gap) {
    int idx = t->tip_start - 1;
    double v = gap < 0.05 ? 0.05 : gap;
    if (v > t->target_diameter[idx]) v = t->target_diameter[idx];
    t->target_diameter[idx] = v;
}

void wb_tract_set_velum(wb_tract_t *t, double openness) {
    t->velum_target = openness < 0.01 ? 0.01 : openness;
}

void wb_tract_set_constriction(wb_tract_t *t, double index, double diameter, double width) {
    if (index < 2) index = 2;
    if (index > t->n - 1) index = t->n - 1;
    int ii = (int)lround(index);
    int lo = ii - (int)width - 1, hi = ii + (int)width + 1;
    for (int k = lo; k <= hi; k++) {
        if (k < 0 || k >= t->n) continue;
        double relpos = fabs(k - index) - 0.5;
        double shrink;
        if (relpos <= 0) shrink = 0.0;
        else if (relpos > width) shrink = 1.0;
        else shrink = 0.5 * (1 - cos(M_PI * relpos / width));
        if (diameter < t->target_diameter[k]) {
            t->target_diameter[k] = diameter + (t->target_diameter[k] - diameter) * shrink;
        }
    }
}

static void add_transient(wb_tract_t *t, int position) {
    if (t->n_transients >= t->cap_transients) {
        size_t nc = t->cap_transients ? t->cap_transients * 2 : 8;
        void *np = realloc(t->transients, nc * sizeof(*t->transients));
        if (!np) return;
        t->transients = np;
        t->cap_transients = nc;
    }
    t->transients[t->n_transients].position = position;
    t->transients[t->n_transients].time_alive = 0.0;
    t->transients[t->n_transients].life_time = 0.2;
    t->transients[t->n_transients].strength = 0.3;
    t->transients[t->n_transients].exponent = 200;
    t->n_transients++;
}

static void process_transients(wb_tract_t *t) {
    size_t w = 0;
    for (size_t i = 0; i < t->n_transients; i++) {
        double amp = t->transients[i].strength * pow(2, -t->transients[i].exponent * t->transients[i].time_alive);
        int p = t->transients[i].position;
        if (p < t->n) { t->R[p] += amp / 2; t->L[p] += amp / 2; }
        t->transients[i].time_alive += 1.0 / (44100.0 * 2);
        if (t->transients[i].time_alive <= t->transients[i].life_time) {
            t->transients[w++] = t->transients[i];
        }
    }
    t->n_transients = w;
}

static void add_turbulence_at(wb_tract_t *t, double noise, double index, double diameter) {
    int i = (int)floor(index);
    double delta = index - i;
    double thinness = clampd(8 * (0.7 - diameter), 0, 1);
    double openness = clampd(30 * (diameter - 0.3), 0, 1);
    double n0 = noise * (1 - delta) * thinness * openness;
    double n1 = noise * delta * thinness * openness;
    if (i + 1 < t->n) { t->R[i + 1] += n0 / 2; t->L[i + 1] += n0 / 2; }
    if (i + 2 < t->n) { t->R[i + 2] += n1 / 2; t->L[i + 2] += n1 / 2; }
}

static void add_turbulence(wb_tract_t *t, double noise) {
    /* R013 mouth: the frication source sits where the constriction actually
     * is. Labiodental /f v/ inject at the lips; sibilants inject at the
     * teeth; stops inject at their place of articulation. Default (noise_index
     * < 0) is the alveolar tip, preserving the original Pink Trombone path.
     * The front cavity between source and lips then sizes the spectral peak. */
    int idx = t->noise_index < 0 ? t->tip_start
                                 : (int)clampd(t->noise_index, 2, t->n - 2);
    double d = t->diameter[idx];
    if (d > 0) add_turbulence_at(t, 0.66 * noise, idx, d);
}

static void reshape(wb_tract_t *t, double dt) {
    double amount = dt * t->movement_speed;
    int new_last_obstruction = -1;
    for (int i = 0; i < t->n; i++) {
        double d = t->diameter[i];
        double tgt = t->target_diameter[i];
        if (d <= 0) new_last_obstruction = i;
        double slow_return;
        if (i < t->nose_start) slow_return = 0.6;
        else if (i >= t->tip_start) slow_return = 1.0;
        else slow_return = 0.6 + 0.4 * (i - t->nose_start) / (double)(t->tip_start - t->nose_start);
        t->diameter[i] = move_towards(d, tgt, slow_return * amount, 2 * amount);
    }
    if (t->last_obstruction > -1 && new_last_obstruction == -1 && t->nose_A[0] < 0.05) {
        add_transient(t, t->last_obstruction);
    }
    t->last_obstruction = new_last_obstruction;

    amount = dt * t->movement_speed;
    t->nose_diameter[0] = move_towards(t->nose_diameter[0], t->velum_target, amount * 0.25, amount * 0.1);
    t->nose_A[0] = t->nose_diameter[0] * t->nose_diameter[0];
}

static void calculate_reflections(wb_tract_t *t) {
    for (int i = 0; i < t->n; i++) t->A[i] = t->diameter[i] * t->diameter[i];
    for (int i = 1; i < t->n; i++) {
        t->reflection[i] = t->new_reflection[i];
        if (t->A[i] == 0) t->new_reflection[i] = 0.999;
        else t->new_reflection[i] = (t->A[i-1] - t->A[i]) / (t->A[i-1] + t->A[i]);
    }
    double s = t->A[t->nose_start] + t->A[t->nose_start + 1] + t->nose_A[0];
    t->reflection_left = t->new_reflection_left;
    t->reflection_right = t->new_reflection_right;
    t->reflection_nose = t->new_reflection_nose;
    t->new_reflection_left  = (2 * t->A[t->nose_start]     - s) / s;
    t->new_reflection_right = (2 * t->A[t->nose_start + 1] - s) / s;
    t->new_reflection_nose  = (2 * t->nose_A[0]            - s) / s;
}

static void calculate_nose_reflections(wb_tract_t *t) {
    for (int i = 0; i < t->nose_length; i++) t->nose_A[i] = t->nose_diameter[i] * t->nose_diameter[i];
    for (int i = 1; i < t->nose_length; i++) {
        t->nose_reflection[i] = (t->nose_A[i-1] - t->nose_A[i]) / (t->nose_A[i-1] + t->nose_A[i]);
    }
}

double wb_tract_run_step(wb_tract_t *t, double glottal_output, double turbulence_noise, double lambda) {
    process_transients(t);
    add_turbulence(t, turbulence_noise);

    t->junction_R[0] = t->L[0] * t->glottal_reflection + glottal_output;
    t->junction_L[t->n] = t->R[t->n - 1] * t->lip_reflection;

    for (int i = 1; i < t->n; i++) {
        double r = t->reflection[i] * (1 - lambda) + t->new_reflection[i] * lambda;
        double w = r * (t->R[i-1] + t->L[i]);
        t->junction_R[i] = t->R[i-1] - w;
        t->junction_L[i] = t->L[i] + w;
    }

    /* nose junction (3-way split) */
    int i = t->nose_start;
    double r = t->new_reflection_left * (1 - lambda) + t->reflection_left * lambda;
    t->junction_L[i] = r * t->R[i-1] + (1 + r) * (t->nose_L[0] + t->L[i]);
    r = t->new_reflection_right * (1 - lambda) + t->reflection_right * lambda;
    t->junction_R[i] = r * t->L[i] + (1 + r) * (t->R[i-1] + t->nose_L[0]);
    r = t->new_reflection_nose * (1 - lambda) + t->reflection_nose * lambda;
    t->nose_junction_R[0] = r * t->nose_L[0] + (1 + r) * (t->L[i] + t->R[i-1]);

    for (int j = 0; j < t->n; j++) {
        t->R[j] = clampd(t->junction_R[j] * 0.999, -1, 1);
        t->L[j] = clampd(t->junction_L[j+1] * 0.999, -1, 1);
    }
    t->lip_output = t->R[t->n - 1];

    /* nose */
    t->nose_junction_L[t->nose_length] = t->nose_R[t->nose_length - 1] * t->lip_reflection;
    for (int j = 1; j < t->nose_length; j++) {
        double w = t->nose_reflection[j] * (t->nose_R[j-1] + t->nose_L[j]);
        t->nose_junction_R[j] = t->nose_R[j-1] - w;
        t->nose_junction_L[j] = t->nose_L[j] + w;
    }
    for (int j = 0; j < t->nose_length; j++) {
        t->nose_R[j] = clampd(t->nose_junction_R[j] * 0.999, -1, 1);
        t->nose_L[j] = clampd(t->nose_junction_L[j+1] * 0.999, -1, 1);
    }
    t->nose_output = t->nose_R[t->nose_length - 1];

    return t->lip_output + t->nose_output;
}

void wb_tract_finish_block(wb_tract_t *t, double block_time) {
    reshape(t, block_time);
    calculate_reflections(t);
    calculate_nose_reflections(t);
}

int wb_tract_n(const wb_tract_t *t) { return t->n; }

void wb_tract_set_diameter(wb_tract_t *t, int idx, double d) {
    if (idx < 0 || idx >= t->n) return;
    if (d < 0.05) d = 0.05;
    if (d > 3.0) d = 3.0;
    t->target_diameter[idx] = d;
}

double wb_tract_get_diameter(const wb_tract_t *t, int idx) {
    if (idx < 0 || idx >= t->n) return 0.0;
    return t->target_diameter[idx];
}

void wb_tract_set_all_diameters(wb_tract_t *t, const double *diams, int n) {
    int lim = n < t->n ? n : t->n;
    for (int i = 0; i < lim; i++) {
        double d = diams[i];
        if (d < 0.05) d = 0.05;
        if (d > 3.0) d = 3.0;
        t->target_diameter[i] = d;
    }
}

void wb_tract_get_all_diameters(const wb_tract_t *t, double *diams, int n) {
    int lim = n < t->n ? n : t->n;
    for (int i = 0; i < lim; i++) diams[i] = t->target_diameter[i];
}
