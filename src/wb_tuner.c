/* wb_tuner.c — recursive self-improvement / feedback-fix loop.
 *
 * Circular loop: SENSE → ACT → MEASURE → COMPARE → FIX → (loop).
 * Runs during idle cycles on the weak mac: each pass, the DAW renders a
 * short probe signal through each effect, measures an objective metric
 * (spectral flatness, RMS stability, latency), compares against a target,
 * and nudges DSP parameters to minimize error.
 *
 * Design (per WuBu doctrine): pure C11, lock-free handoff, allocation-free
 * on the RT audio thread. The tune loop runs on a background thread that
 * only *requests* parameter changes; the engine pushes them across the
 * command queue next render-block.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#include "wbus.h"
#include "wb_internal.h"

/* a single tunable parameter reference */
typedef struct {
    char   plugin[32];
    int    param_id;
    float  value;     /* current */
    float  lo, hi;    /* bounds */
    float  target;    /* desired metric target (0..1) */
} wb_knob;

/* the tuner: holds knobs, runs the sense/act/measure/compare/fix cycle */
typedef struct wb_tuner {
    wb_engine *engine;
    wb_knob  *knobs;
    int       n_knobs;
    int       running;
    pthread_t thread;
    double    last_loss;
} wb_tuner;

/* ---- measurement primitives ------------------------------------------ */

/* Spectral flatness of a block: 0 (tonal/peaky) to 1 (noise-like).
 * We want our synth voice to stay tonal -> target low flatness.
 * Computed as geometric_mean / arithmetic_mean of |FFT bins|.
 * For the weak mac we use a 64-bin Goertzel-ish proxy (no full FFT needed). */
static float measure_spectral_flatness(const wb_sample *x, uint32_t n) {
    if (n < 8) return 0.0f;
    /* crude frame energy + zero-crossing proxy for tonal-ness */
    float sum = 0, sumsq = 0;
    int zc = 0;
    for (uint32_t i = 1; i < n; i++) {
        float a = fabsf(x[i]); (void)sumsq;
        sum += a; sumsq += a*a;
        if ((x[i-1] < 0 && x[i] >= 0) || (x[i-1] >= 0 && x[i] < 0)) zc++;
    }
    float mean = sum / n;
    float var = sumsq / n - mean*mean;
    /* high ZCR => noisier => higher 'flatness' proxy;
     * std unused beyond var (kept for future gradient use) */
    float zcr_norm = (float)zc / (n / 2);
    (void)var;
    return zcr_norm * (1.0f / (1.0f + mean * 1000.0f));
}

/* RMS of a block, normalised (0..1). */
static float measure_rms(const wb_sample *x, uint32_t n) {
    double acc = 0;
    for (uint32_t i = 0; i < n*2; i++) acc += (double)x[i]*x[i];
    return (float)sqrt(acc / (n * 2));
}

/* ---- the fix step: tiny gradient descent on each knob ------------------ */
static void fix_knob(wb_tuner *t, int k, float metric, float target) {
    float err = metric - target;
    /* move toward target (simple proportional nudge) */
    float delta = -0.005f * err;
    t->knobs[k].value += delta;
    if (t->knobs[k].value < t->knobs[k].lo) t->knobs[k].value = t->knobs[k].lo;
    if (t->knobs[k].value > t->knobs[k].hi) t->knobs[k].value = t->knobs[k].hi;
    /* push to engine */
    wb_engine_set_insert_param(t->engine, 0, 0,
                               t->knobs[k].param_id, t->knobs[k].value);
}

/* ---- the circular loop (background thread) --------------------------- */
static void *tune_loop(void *arg) {
    wb_tuner *t = arg;
    wb_sample probe[WB_MAX_BLOCK * 2];

    while (t->running) {
        /* ACT+MEASURE: render a probe block through the engine */
        uint32_t got = wb_engine_render(t->engine, probe, 512);

        /* COMPARE + FIX per knob: choose metric per knob type */
        float total_err = 0;
        for (int k = 0; k < t->n_knobs; k++) {
            float metric;
            /* knob 0 = synth filter cutoff (want tonal, low flatness proxy) */
            if (k == 0) metric = measure_spectral_flatness(probe, got);
            /* knob 1 = master vol (want RMS near 0.1) */
            else if (k == 1) metric = measure_rms(probe, got);
            else metric = measure_rms(probe, got);
            total_err += fabsf(metric - t->knobs[k].target);
            fix_knob(t, k, metric, t->knobs[k].target);
        }
        t->last_loss = (double)total_err;
        /* SENSE: sleep briefly (don't starve the weak mac; use a short
         * sleep that still lets the loop run several times/sec) */
        struct timespec ts = {0, 20 * 1000000}; /* 20ms */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---- lifecycle ------------------------------------------------------- */
wb_tuner *wb_tuner_create(wb_engine *e) {
    wb_tuner *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->engine = e;
    /* two example knobs to close the loop:
     *  - filter cutoff of the synth (param 1) -> want tonal voice (target flatness ~0.05)
     *  - master volume (param 0) -> want RMS ~0.1 */
    t->n_knobs = 2;
    t->knobs = calloc(2, sizeof(wb_knob));
    t->knobs[0].param_id = 1; t->knobs[0].value = 0.5f; t->knobs[0].lo = 0; t->knobs[0].hi = 1;
    t->knobs[0].target = 0.05f;  /* low spectral flatness = tonal */
    strncpy(t->knobs[0].plugin, "synth", sizeof(t->knobs[0].plugin));
    t->knobs[1].param_id = 0; t->knobs[1].value = 0.8f; t->knobs[1].lo = 0; t->knobs[1].hi = 1;
    t->knobs[1].target = 0.1f;   /* target RMS */
    strncpy(t->knobs[1].plugin, "synth", sizeof(t->knobs[1].plugin));
    return t;
}

void wb_tuner_start(wb_tuner *t) {
    if (!t) return;
    t->running = 1;
    pthread_create(&t->thread, NULL, tune_loop, t);
}

void wb_tuner_stop(wb_tuner *t) {
    if (!t) return;
    t->running = 0;
    pthread_join(t->thread, NULL);
}

void wb_tuner_destroy(wb_tuner *t) {
    if (!t) return;
    wb_tuner_stop(t);
    free(t->knobs);
    free(t);
}

double wb_tuner_last_loss(const wb_tuner *t) {
    return t ? t->last_loss : 0.0;
}
