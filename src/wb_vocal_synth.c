/* wb_vocal_synth.c — formant synthesizer (vocal synthesis from phonemes).
 *
 * R078: Vocaloid/Logic-style formant synthesis.
 *
 * 5 formants (F1-F5) with parallel bandpass filters.
 * Glottal source: LF-model pulse train.
 * Vowel morphing: interpolate formant frequencies.
 * Pure C11. */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wbus.h"

#define NUM_FORMANTS 5
#define NUM_VOWELS 5

/* Vowel formant frequencies: ah, eh, ee, oh, oo */
static const float formant_freqs[NUM_VOWELS][NUM_FORMANTS] = {
    {700,  1100, 2400, 3200, 4000},  /* ah */
    {500,  1800, 2500, 3300, 4200},  /* eh */
    {300,  2200, 2900, 3500, 4500},  /* ee */
    {450,   900, 2400, 3200, 4000},  /* oh */
    {350,   700, 2400, 3000, 3800},  /* oo */
};

static const float formant_bw[NUM_FORMANTS] = {80, 90, 100, 120, 140};
static const float formant_gain[NUM_FORMANTS] = {1.0f, 0.8f, 0.6f, 0.4f, 0.3f};

/* Biquad bandpass state */
typedef struct {
    float x[3];  /* x[n], x[n-1], x[n-2] */
    float y[3];  /* y[n], y[n-1], y[n-2] */
    float b0, b1, b2, a1, a2;
} biquad_t;

static void biquad_init(biquad_t *bq) {
    memset(bq, 0, sizeof(*bq));
}

static void biquad_set_bandpass(biquad_t *bq, float freq, float bw, uint32_t sr) {
    float w0 = 2.0f * M_PI * freq / (float)sr;
    float sin_w0 = sinf(w0);
    float cos_w0 = cosf(w0);
    float alpha = sin_w0 * sinhf(M_LN2 / 2.0f * bw * w0 / sin_w0);
    float a0 = 1.0f + alpha;
    bq->b0 = alpha / a0;
    bq->b1 = 0;
    bq->b2 = -alpha / a0;
    bq->a1 = -2.0f * cos_w0 / a0;
    bq->a2 = (1.0f - alpha) / a0;
}

static inline float biquad_process(biquad_t *bq, float in) {
    bq->x[0] = in;
    bq->y[0] = bq->b0 * bq->x[0] + bq->b1 * bq->x[1] + bq->b2 * bq->x[2]
               - bq->a1 * bq->y[1] - bq->a2 * bq->y[2];
    bq->x[2] = bq->x[1]; bq->x[1] = bq->x[0];
    bq->y[2] = bq->y[1]; bq->y[1] = bq->y[0];
    return bq->y[0];
}

/* LF-model glottal pulse generator */
typedef struct {
    float phase;
    float freq;
    uint32_t sr;
} glottal_t;

static void glottal_init(glottal_t *g, uint32_t sr) {
    g->phase = 0;
    g->freq = 130.0f; /* default ~C3 */
    g->sr = sr;
}

static void glottal_set_freq(glottal_t *g, float freq) {
    g->freq = freq > 20 ? freq : 20;
}

/* LF-model pulse: asymmetric triangle with exponential decay */
static inline float glottal_pulse(glottal_t *g) {
    float t = g->phase / (float)g->sr;       /* time in seconds */
    float period = 1.0f / g->freq;
    float tp = 0.4f * period;                 /* open phase */
    float te = 0.6f * period;                 /* closed phase */
    float local_t = fmodf(t, period);
    float out;
    if (local_t < tp) {
        /* Opening + closing: parabolic */
        float x = local_t / tp;
        out = 3.0f * x * x - 2.0f * x * x * x; /* smoothstep-like */
    } else {
        /* Exponential decay */
        float decay_t = (local_t - tp) / te;
        out = expf(-3.0f * decay_t);
    }
    g->phase += 1.0f;
    if (g->phase >= (float)g->sr) g->phase -= (float)g->sr;
    return out;
}

typedef struct {
    uint32_t sr;
    glottal_t glottal;
    biquad_t formants[NUM_FORMANTS];
    float vowel_pos;     /* 0..1 mapped to vowel index */
    float breathiness;
    int midi_note;
    int active;
    float current_freqs[NUM_FORMANTS];
} wb_vocal_synth;

void *wb_vocal_synth_create(uint32_t sr) {
    wb_vocal_synth *v = (wb_vocal_synth *)calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->sr = sr;
    glottal_init(&v->glottal, sr);
    v->vowel_pos = 0;
    v->breathiness = 0;
    v->midi_note = 60; /* middle C */
    v->active = 0;
    for (int i = 0; i < NUM_FORMANTS; i++) {
        biquad_init(&v->formants[i]);
        v->current_freqs[i] = formant_freqs[0][i];
        biquad_set_bandpass(&v->formants[i], formant_freqs[0][i], formant_bw[i], sr);
    }
    return v;
}

void wb_vocal_synth_destroy(void *inst) {
    free(inst);
}

static void update_formants(wb_vocal_synth *v) {
    /* Map vowel_pos 0..1 to vowel index */
    float vf = v->vowel_pos * (NUM_VOWELS - 1);
    int v0 = (int)vf;
    int v1 = v0 + 1;
    if (v1 >= NUM_VOWELS) v1 = NUM_VOWELS - 1;
    float frac = vf - v0;
    for (int i = 0; i < NUM_FORMANTS; i++) {
        v->current_freqs[i] = formant_freqs[v0][i] * (1.0f - frac) + formant_freqs[v1][i] * frac;
        biquad_set_bandpass(&v->formants[i], v->current_freqs[i], formant_bw[i], v->sr);
    }
}

void wb_vocal_synth_set_vowel(void *inst, float vowel_position) {
    wb_vocal_synth *v = (wb_vocal_synth *)inst;
    if (!v) return;
    v->vowel_pos = vowel_position < 0 ? 0 : (vowel_position > 1 ? 1 : vowel_position);
    update_formants(v);
}

void wb_vocal_synth_set_pitch(void *inst, int midi_note) {
    wb_vocal_synth *v = (wb_vocal_synth *)inst;
    if (!v) return;
    v->midi_note = midi_note;
    float freq = 440.0f * powf(2.0f, (midi_note - 69) / 12.0f);
    glottal_set_freq(&v->glottal, freq);
    v->active = 1;
}

void wb_vocal_synth_speak(void *inst, const char *phonemes) {
    wb_vocal_synth *v = (wb_vocal_synth *)inst;
    if (!v || !phonemes) return;
    /* Parse simple phoneme strings: "ah", "eh", "ee", "oh", "oo" */
    v->active = 1;
    /* Set initial vowel based on first phoneme */
    if (strncmp(phonemes, "ah", 2) == 0) v->vowel_pos = 0.0f;
    else if (strncmp(phonemes, "eh", 2) == 0) v->vowel_pos = 0.25f;
    else if (strncmp(phonemes, "ee", 2) == 0) v->vowel_pos = 0.5f;
    else if (strncmp(phonemes, "oh", 2) == 0) v->vowel_pos = 0.75f;
    else if (strncmp(phonemes, "oo", 2) == 0) v->vowel_pos = 1.0f;
    else v->vowel_pos = 0;
    update_formants(v);
}

void wb_vocal_synth_set_breathiness(void *inst, float amount) {
    wb_vocal_synth *v = (wb_vocal_synth *)inst;
    if (!v) return;
    v->breathiness = amount < 0 ? 0 : (amount > 1 ? 1 : amount);
}

void wb_vocal_synth_render(void *inst, wb_sample *out, uint32_t frames) {
    wb_vocal_synth *v = (wb_vocal_synth *)inst;
    if (!v || !out) return;

    if (!v->active) {
        memset(out, 0, frames * sizeof(wb_sample));
        return;
    }

    for (uint32_t i = 0; i < frames; i++) {
        /* Glottal source */
        float glottal = glottal_pulse(&v->glottal);
        /* Breathiness (filtered noise) */
        float noise = ((float)(rand() % 2000) / 1000.0f - 1.0f) * v->breathiness * 0.3f;
        float source = glottal + noise;
        /* Parallel formant filters */
        float vocal = 0;
        for (int f = 0; f < NUM_FORMANTS; f++) {
            vocal += biquad_process(&v->formants[f], source) * formant_gain[f];
        }
        out[i] = vocal * 0.3f; /* scale to avoid clipping */
    }
}
