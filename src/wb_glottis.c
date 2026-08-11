/*
 * wb_glottis.c — LF glottal flow model (strict C11)
 */
#include "wb_glottis.h"

#include <stdlib.h>
#include <math.h>

struct wb_glottis {
    double time_in_waveform;
    double old_frequency, new_frequency;
    double ui_frequency, smooth_frequency;
    double old_tenseness, new_tenseness;
    double ui_tenseness;
    double total_time;
    double vibrato_amount, vibrato_frequency;
    double jitter, shimmer;
    double intensity, loudness;
    double waveform_length;
    /* LF params */
    double alpha, E0, epsilon, shift, Delta, Te, omega;
    double Rd;
    double phase;
};

static void wb_setup_waveform(wb_glottis_t *g, double lambda);

wb_glottis_t *wb_glottis_new(void) {
    wb_glottis_t *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->old_frequency = g->new_frequency = g->ui_frequency = g->smooth_frequency = 140.0;
    g->old_tenseness = g->new_tenseness = g->ui_tenseness = 0.6;
    g->vibrato_amount = 0.005;
    g->vibrato_frequency = 6.0;
    g->jitter = 0.005;
    g->shimmer = 0.02;
    g->intensity = 0.0;
    g->loudness = 1.0;
    g->waveform_length = 1.0 / 140.0;
    wb_setup_waveform(g, 0.0);  /* prime the LF params so the first samples are finite */
    return g;
}

void wb_glottis_free(wb_glottis_t *g) { free(g); }

void wb_glottis_set_frequency(wb_glottis_t *g, double hz) { g->ui_frequency = hz; }
void wb_glottis_set_tenseness(wb_glottis_t *g, double t) { g->ui_tenseness = t; }
void wb_glottis_set_vibrato(wb_glottis_t *g, double depth, double rate) {
    g->vibrato_amount = depth; g->vibrato_frequency = rate;
}
void wb_glottis_set_jitter(wb_glottis_t *g, double a) { g->jitter = a; }
void wb_glottis_set_shimmer(wb_glottis_t *g, double a) { g->shimmer = a; }
void wb_glottis_set_intensity(wb_glottis_t *g, double i) { g->intensity = i; }

static double lcg(void) {
    /* deterministic LCG for jitter/shimmer — no libc rand, no third party */
    static unsigned long long s = 0x9E3779B97F4A7C15ULL;
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((s >> 33) & 0xFFFFFF) / 16777215.0;
}

/* We inline the waveform setup into run_step via this helper. */
static void wb_setup_waveform(wb_glottis_t *g, double lambda) {
    g->old_frequency = g->new_frequency; /* glide base */
    double freq = g->old_frequency * (1 - lambda) + g->new_frequency * lambda;
    double tenseness = g->old_tenseness * (1 - lambda) + g->new_tenseness * lambda;
    g->Rd = 3 * (1 - tenseness);
    g->waveform_length = 1.0 / (freq < 30 ? 30 : freq);

    double Rd = g->Rd; if (Rd < 0.5) Rd = 0.5; if (Rd > 2.7) Rd = 2.7;
    double Ra = -0.01 + 0.048 * Rd;
    double Rk = 0.224 + 0.118 * Rd;
    double Rg = (Rk / 4) * (0.5 + 1.2 * Rk) / (0.11 * Rd - Ra * (0.5 + 1.2 * Rk));
    double Ta = Ra;
    double Tp = 1 / (2 * Rg);
    double Te = Tp + Tp * Rk;
    double epsilon = 1 / Ta;
    double shift = exp(-epsilon * (1 - Te));
    double Delta = 1 - shift;
    double RHS = (1 / epsilon) * (shift - 1) + (1 - Te) * shift;
    RHS /= Delta;
    double total_upper = (Te - Tp) / 2 - RHS;
    double omega = M_PI / Tp;
    double s = sin(omega * Te);
    double y = -M_PI * s * total_upper / (Tp * 2);
    double z = log(y);
    double alpha = z / (Tp / 2 - Te);
    double E0 = -1 / (s * exp(alpha * Te));
    g->alpha = alpha; g->E0 = E0; g->epsilon = epsilon;
    g->shift = shift; g->Delta = Delta; g->Te = Te; g->omega = omega;
}

static double normalized_lf(wb_glottis_t *g, double t) {
    double out;
    if (t > g->Te)
        out = (-exp(-g->epsilon * (t - g->Te)) + g->shift) / g->Delta;
    else
        out = g->E0 * exp(g->alpha * t) * sin(g->omega * t);
    return out * g->intensity * g->loudness;
}

double wb_glottis_run_step(wb_glottis_t *g, double lambda, double aspiration_noise) {
    double time_step = 1.0 / 44100.0;
    g->time_in_waveform += time_step;
    g->total_time += time_step;
    if (g->time_in_waveform > g->waveform_length) {
        g->time_in_waveform -= g->waveform_length;
        wb_setup_waveform(g, lambda);
    }
    double out = normalized_lf(g, g->time_in_waveform / g->waveform_length);

    /* aspiration (breathy) */
    double voiced = 0.1 + 0.2 * fmax(0, sin(2 * M_PI * g->time_in_waveform / g->waveform_length));
    double noise_mod = g->ui_tenseness * g->intensity * voiced
                     + (1 - g->ui_tenseness * g->intensity) * 0.3;
    double asp = g->intensity * (1 - sqrt(g->ui_tenseness)) * noise_mod * aspiration_noise;
    asp *= 0.2 + 0.02 * (2 * lcg() - 1);
    out += asp;
    return out;
}

void wb_glottis_finish_block(wb_glottis_t *g, int voiced, double block_time) {
    double vib = g->vibrato_amount * sin(2 * M_PI * g->total_time * g->vibrato_frequency);
    if (g->ui_frequency > g->smooth_frequency)
        g->smooth_frequency = fmin(g->smooth_frequency * 1.1, g->ui_frequency);
    else if (g->ui_frequency < g->smooth_frequency)
        g->smooth_frequency = fmax(g->smooth_frequency / 1.1, g->ui_frequency);
    g->old_frequency = g->new_frequency;
    g->new_frequency = g->smooth_frequency * (1 + vib);
    g->old_tenseness = g->new_tenseness;
    g->new_tenseness = g->ui_tenseness
        + 0.1 * sin(g->total_time * 0.46) + 0.05 * sin(g->total_time * 0.36);
    if (voiced || g->intensity > 0.25) g->intensity += 0.13;
    else g->intensity -= block_time * 5;
    if (g->intensity < 0) g->intensity = 0;
    if (g->intensity > 1) g->intensity = 1;
}
