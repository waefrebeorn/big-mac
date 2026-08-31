/* wb_mod_matrix.c — Bitwig-style modular modulation routing matrix.
 *
 * Routes modulation sources (LFOs, envelope, MIDI controllers) to parameter
 * destinations with bipolar amount scaling. Up to 64 concurrent routes.
 *
 * Sources: LFO1, LFO2, Envelope, Velocity, ModWheel, PitchBend, Aftertouch
 * Destinations: parameter indices 0..127
 * Amount: -1.0 .. +1.0 (bipolar)
 */

#include "wbus.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WB_MOD_MATRIX_MAX_ROUTES 64
#define WB_MOD_MATRIX_MAX_PARAMS 128

/* Modulation source IDs */
#define WB_MOD_SRC_LFO1       0
#define WB_MOD_SRC_LFO2       1
#define WB_MOD_SRC_ENVELOPE   2
#define WB_MOD_SRC_VELOCITY   3
#define WB_MOD_SRC_MODWHEEL   4
#define WB_MOD_SRC_PITCHBEND  5
#define WB_MOD_SRC_AFTERTOUCH 6
#define WB_MOD_SRC_COUNT      7

/* LFO waveform shapes */
#define WB_MOD_LFO_SINE     0
#define WB_MOD_LFO_TRIANGLE 1
#define WB_MOD_LFO_SAW      2
#define WB_MOD_LFO_SQUARE   3

/* ADSR stages */
#define WB_MOD_ENV_IDLE     0
#define WB_MOD_ENV_ATTACK   1
#define WB_MOD_ENV_DECAY    2
#define WB_MOD_ENV_SUSTAIN  3
#define WB_MOD_ENV_RELEASE  4

/* Per-route definition */
typedef struct wb_mod_route {
    int   active;
    int   id;
    int   src;
    int   dst;
    float amount;
} wb_mod_route;

/* LFO state */
typedef struct wb_mod_lfo {
    int   waveform;     /* WB_MOD_LFO_* */
    float freq;         /* Hz, 0.1 .. 20.0 */
    float phase;        /* current phase 0..1 */
    float sr;           /* sample rate */
} wb_mod_lfo;

/* ADSR envelope state */
typedef struct wb_mod_env {
    int   stage;        /* WB_MOD_ENV_* */
    float level;        /* current output 0..1 */
    float a, d, s, r;   /* times in seconds */
    float sr;
} wb_mod_env;

/* MIDI/controller state (set externally before process) */
typedef struct wb_mod_midi {
    float velocity;     /* 0..1 (normalized from 0-127) */
    float modwheel;     /* 0..1 */
    float pitchbend;    /* -1..1 (normalized) */
    float aftertouch;   /* 0..1 */
} wb_mod_midi;

/* The modulation matrix */
struct wb_mod_matrix {
    wb_mod_route routes[WB_MOD_MATRIX_MAX_ROUTES];
    int          route_count;
    int          next_route_id;

    /* Sources */
    wb_mod_lfo lfo1;
    wb_mod_lfo lfo2;
    wb_mod_env env;
    wb_mod_midi midi;

    float sr;
};
typedef struct wb_mod_matrix wb_mod_matrix;

/* ---- internal helpers ---------------------------------------------------- */

static float wb_mod_lfo_next(wb_mod_lfo *lfo) {
    float phase = lfo->phase;
    float out;

    switch (lfo->waveform) {
    case WB_MOD_LFO_SINE:
        out = (float)sin((double)(phase * 2.0 * M_PI));
        break;
    case WB_MOD_LFO_TRIANGLE: {
        /* Triangle: ramps 0..1..0 over phase 0..1 */
        if (phase < 0.5f)
            out = 4.0f * phase - 1.0f;       /* -1 at 0, +1 at 0.5 */
        else
            out = 3.0f - 4.0f * phase;        /* +1 at 0.5, -1 at 1 */
        break;
    }
    case WB_MOD_LFO_SAW:
        out = 2.0f * phase - 1.0f;            /* -1..+1 ramp */
        break;
    case WB_MOD_LFO_SQUARE:
        out = (phase < 0.5f) ? 1.0f : -1.0f;
        break;
    default:
        out = 0.0f;
        break;
    }

    /* Advance phase */
    lfo->phase += lfo->freq / lfo->sr;
    if (lfo->phase >= 1.0f) lfo->phase -= 1.0f;

    return out;
}

static float wb_mod_env_next(wb_mod_env *env) {
    switch (env->stage) {
    case WB_MOD_ENV_IDLE:
        env->level = 0.0f;
        break;
    case WB_MOD_ENV_ATTACK: {
        float inc = 1.0f / (env->a * env->sr + 1e-9f);
        env->level += inc;
        if (env->level >= 1.0f) {
            env->level = 1.0f;
            env->stage = WB_MOD_ENV_DECAY;
        }
        break;
    }
    case WB_MOD_ENV_DECAY: {
        if (env->d > 0.0f) {
            float coeff = 1.0f - (1.0f / (env->d * env->sr + 1e-9f));
            env->level += (env->s - env->level) * (1.0f - coeff);
            if (env->level <= env->s + 1e-4f) {
                env->level = env->s;
                env->stage = WB_MOD_ENV_SUSTAIN;
            }
        } else {
            env->level = env->s;
            env->stage = WB_MOD_ENV_SUSTAIN;
        }
        break;
    }
    case WB_MOD_ENV_SUSTAIN:
        env->level = env->s;
        break;
    case WB_MOD_ENV_RELEASE: {
        if (env->r > 0.0f) {
            float coeff = 1.0f - (1.0f / (env->r * env->sr + 1e-9f));
            env->level *= coeff;
            if (env->level < 1e-4f) {
                env->level = 0.0f;
                env->stage = WB_MOD_ENV_IDLE;
            }
        } else {
            env->level = 0.0f;
            env->stage = WB_MOD_ENV_IDLE;
        }
        break;
    }
    }
    return env->level;
}

static float wb_mod_get_source_value(wb_mod_matrix *mm, int src) {
    switch (src) {
    case WB_MOD_SRC_LFO1:      return wb_mod_lfo_next(&mm->lfo1);
    case WB_MOD_SRC_LFO2:      return wb_mod_lfo_next(&mm->lfo2);
    case WB_MOD_SRC_ENVELOPE:  return wb_mod_env_next(&mm->env);
    case WB_MOD_SRC_VELOCITY:  return mm->midi.velocity;
    case WB_MOD_SRC_MODWHEEL:  return mm->midi.modwheel * 2.0f - 1.0f; /* 0..1 -> -1..1 */
    case WB_MOD_SRC_PITCHBEND: return mm->midi.pitchbend;
    case WB_MOD_SRC_AFTERTOUCH:return mm->midi.aftertouch * 2.0f - 1.0f; /* 0..1 -> -1..1 */
    default:                   return 0.0f;
    }
}

static int wb_mod_find_route(wb_mod_matrix *mm, int route_id) {
    for (int i = 0; i < WB_MOD_MATRIX_MAX_ROUTES; i++) {
        if (mm->routes[i].active && mm->routes[i].id == route_id)
            return i;
    }
    return -1;
}

/* ---- public API ---------------------------------------------------------- */

void *wb_mod_matrix_create(void) {
    wb_mod_matrix *mm = (wb_mod_matrix *)calloc(1, sizeof(wb_mod_matrix));
    if (!mm) return NULL;

    mm->sr = 44100.0f;
    mm->route_count = 0;
    mm->next_route_id = 1;

    /* Default LFO settings */
    mm->lfo1.waveform = WB_MOD_LFO_SINE;
    mm->lfo1.freq = 2.0f;
    mm->lfo1.phase = 0.0f;
    mm->lfo1.sr = mm->sr;

    mm->lfo2.waveform = WB_MOD_LFO_SINE;
    mm->lfo2.freq = 0.5f;
    mm->lfo2.phase = 0.0f;
    mm->lfo2.sr = mm->sr;

    /* Default envelope */
    mm->env.stage = WB_MOD_ENV_IDLE;
    mm->env.level = 0.0f;
    mm->env.a = 0.01f;
    mm->env.d = 0.1f;
    mm->env.s = 0.7f;
    mm->env.r = 0.3f;
    mm->env.sr = mm->sr;

    /* Default MIDI state */
    mm->midi.velocity = 0.0f;
    mm->midi.modwheel = 0.0f;
    mm->midi.pitchbend = 0.0f;
    mm->midi.aftertouch = 0.0f;

    return (void *)mm;
}

void wb_mod_matrix_destroy(void *mm) {
    free(mm);
}

int wb_mod_matrix_add_route(void *mm, int src, int dst, float amount) {
    wb_mod_matrix *m = (wb_mod_matrix *)mm;
    if (!m) return -1;
    if (src < 0 || src >= WB_MOD_SRC_COUNT) return -1;
    if (dst < 0 || dst >= WB_MOD_MATRIX_MAX_PARAMS) return -1;
    if (amount < -1.0f || amount > 1.0f) return -1;
    if (m->route_count >= WB_MOD_MATRIX_MAX_ROUTES) return -1;

    /* Find a free slot */
    for (int i = 0; i < WB_MOD_MATRIX_MAX_ROUTES; i++) {
        if (!m->routes[i].active) {
            m->routes[i].active = 1;
            m->routes[i].id = m->next_route_id++;
            m->routes[i].src = src;
            m->routes[i].dst = dst;
            m->routes[i].amount = amount;
            m->route_count++;
            return m->routes[i].id;
        }
    }
    return -1; /* no free slots (shouldn't happen if count check passed) */
}

int wb_mod_matrix_remove_route(void *mm, int route_id) {
    wb_mod_matrix *m = (wb_mod_matrix *)mm;
    if (!m) return -1;
    if (route_id <= 0) return -1;

    int idx = wb_mod_find_route(m, route_id);
    if (idx < 0) return -1;

    m->routes[idx].active = 0;
    m->route_count--;
    return 0;
}

void wb_mod_matrix_set_amount(void *mm, int route_id, float amount) {
    wb_mod_matrix *m = (wb_mod_matrix *)mm;
    if (!m) return;
    if (amount < -1.0f) amount = -1.0f;
    if (amount > 1.0f) amount = 1.0f;

    int idx = wb_mod_find_route(m, route_id);
    if (idx >= 0)
        m->routes[idx].amount = amount;
}

void wb_mod_matrix_process(void *mm, float *param_values, int num_params) {
    wb_mod_matrix *m = (wb_mod_matrix *)mm;
    if (!m || !param_values || num_params <= 0) return;

    for (int i = 0; i < WB_MOD_MATRIX_MAX_ROUTES; i++) {
        if (!m->routes[i].active) continue;

        int dst = m->routes[i].dst;
        if (dst >= num_params) continue;

        float src_val = wb_mod_get_source_value(m, m->routes[i].src);
        float modulated = src_val * m->routes[i].amount;

        param_values[dst] += modulated;
    }
}

int wb_mod_matrix_route_count(const void *mm) {
    const wb_mod_matrix *m = (const wb_mod_matrix *)mm;
    if (!m) return 0;
    return m->route_count;
}

void wb_mod_matrix_clear(void *mm) {
    wb_mod_matrix *m = (wb_mod_matrix *)mm;
    if (!m) return;
    for (int i = 0; i < WB_MOD_MATRIX_MAX_ROUTES; i++)
        m->routes[i].active = 0;
    m->route_count = 0;
}

/* ---- additional API: configure sources ----------------------------------- */

void wb_mod_matrix_set_lfo(void *mm, int lfo_idx, int waveform, float freq) {
    wb_mod_matrix *m = (wb_mod_matrix *)mm;
    if (!m) return;
    if (freq < 0.1f) freq = 0.1f;
    if (freq > 20.0f) freq = 20.0f;

    wb_mod_lfo *lfo = (lfo_idx == 0) ? &m->lfo1 : &m->lfo2;
    lfo->waveform = waveform;
    lfo->freq = freq;
}

void wb_mod_matrix_note_on(void *mm, float a, float d, float s, float r) {
    wb_mod_matrix *m = (wb_mod_matrix *)mm;
    if (!m) return;
    m->env.a = a;
    m->env.d = d;
    m->env.s = s;
    m->env.r = r;
    m->env.stage = WB_MOD_ENV_ATTACK;
    m->env.level = 0.0f;
}

void wb_mod_matrix_note_off(void *mm) {
    wb_mod_matrix *m = (wb_mod_matrix *)mm;
    if (!m) return;
    if (m->env.stage == WB_MOD_ENV_ATTACK ||
        m->env.stage == WB_MOD_ENV_DECAY ||
        m->env.stage == WB_MOD_ENV_SUSTAIN) {
        m->env.stage = WB_MOD_ENV_RELEASE;
    }
}

void wb_mod_matrix_set_midi(void *mm, float vel, float mw, float pb, float at) {
    wb_mod_matrix *m = (wb_mod_matrix *)mm;
    if (!m) return;
    m->midi.velocity = vel;
    m->midi.modwheel = mw;
    m->midi.pitchbend = pb;
    m->midi.aftertouch = at;
}

/* Evaluate modulation at sample position n, calling setter for each active route */
void wb_mod_matrix_eval(void *mm, uint32_t n, float sample_rate,
                        void (*cb)(void *ctx, int track, int slot, int param, float value01),
                        void *ctx) {
    wb_mod_matrix *m = (wb_mod_matrix *)mm;
    if (!m || !cb) return;

    for (int i = 0; i < m->route_count; i++) {
        wb_mod_route *r = &m->routes[i];
        if (!r->active) continue;

        float value = 0.0f;
        switch (r->src) {
            case WB_MOD_SRC_LFO1:
                value = m->lfo1.phase < 0.5f ? 1.0f : -1.0f; /* square wave */
                break;
            case WB_MOD_SRC_LFO2:
                value = sinf(m->lfo2.phase * 2.0f * M_PI);
                break;
            case WB_MOD_SRC_ENVELOPE:
                value = m->env.level;
                break;
            case WB_MOD_SRC_VELOCITY:
                value = m->midi.velocity;
                break;
            case WB_MOD_SRC_MODWHEEL:
                value = m->midi.modwheel;
                break;
            case WB_MOD_SRC_PITCHBEND:
                value = m->midi.pitchbend;
                break;
            case WB_MOD_SRC_AFTERTOUCH:
                value = m->midi.aftertouch;
                break;
            default:
                break;
        }

        float modulated = value * r->amount;
        cb(ctx, 0, 0, r->dst, modulated);
    }
}