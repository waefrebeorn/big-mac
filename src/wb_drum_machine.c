/* wb_drum_machine.c — TR-808/909-style analog drum synthesis.
 *
 * VST recreation: iconic drum machine voices via analog modeling.
 *
 * Voices:
 *   - BD (bass drum): sine pitch sweep + click + decay (~50Hz fundamental)
 *   - SD (snare): tone (triangle ~180Hz) + noise (filtered white)
 *   - LT/MT/HT (toms): sine pitch sweep + decay, different tunings
 *   - CP (clap): multiple noise bursts + bandpass filter
 *   - RS (rimshot): triangle + noise click
 *   - CB (cowbell): two square waves (600Hz + 800Hz ratio)
 *   - CY (cymbal): multiple square waves + HPF + long decay
 *   - OH/CH (hi-hat): multiple square waves + HPF, open/closed decay
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef enum {
    DRUM_BD = 0,  /* bass drum */
    DRUM_SD,      /* snare */
    DRUM_LT,      /* low tom */
    DRUM_MT,      /* mid tom */
    DRUM_HT,      /* high tom */
    DRUM_CP,      /* clap */
    DRUM_RS,      /* rimshot */
    DRUM_CB,      /* cowbell */
    DRUM_CY,      /* crash */
    DRUM_OH,      /* open hi-hat */
    DRUM_CH,      /* closed hi-hat */
    DRUM_VOICE_COUNT
} drum_voice_t;

typedef struct {
    drum_voice_t voice;
    uint32_t     sr;
    float        pos;        /* current sample position */
    float        len;        /* total length in samples */
    float        amp;        /* amplitude envelope */
    float        env_phase;  /* 0..1 through envelope */
    float        env_step;   /* 1/len */
    float        pitch;      /* oscillator phase */
    float        pitch_inc;  /* oscillator increment */
    float        pitch_sweep;/* pitch sweep amount */
    float        noise_state;/* xorshift state */
    float        pan;
    int          active;
} drum_hit_t;

#define MAX_HITS 32

typedef struct {
    uint32_t sr;
    drum_hit_t hits[MAX_HITS];
    float      volume;
} wb_drum_machine_inst;

static unsigned int rng = 0x9E3779B9u;

static float rng_next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (float)((double)(rng & 0xFFFFFF) / 8388608.0 - 1.0);
}

void *wb_drum_machine_create(uint32_t sr) {
    wb_drum_machine_inst *dm = (wb_drum_machine_inst *)calloc(1, sizeof(*dm));
    if (!dm) return NULL;
    dm->sr = sr;
    dm->volume = 1.0f;
    return dm;
}

void wb_drum_machine_destroy(void *inst) {
    free(inst);
}

/* Configure a drum hit */
static void trigger_drum(wb_drum_machine_inst *dm, drum_voice_t voice, float vel) {
    int slot = -1;
    for (int i = 0; i < MAX_HITS; i++) {
        if (!dm->hits[i].active) { slot = i; break; }
    }
    if (slot < 0) slot = 0; /* steal oldest */

    drum_hit_t *h = &dm->hits[slot];
    memset(h, 0, sizeof(*h));
    h->voice = voice;
    h->sr = dm->sr;
    h->active = 1;
    h->amp = vel;
    h->noise_state = rng;

    switch (voice) {
    case DRUM_BD:
        h->len = (uint32_t)(0.3f * dm->sr);
        h->pitch_inc = 2.0f * 3.14159f * 50.0f / (float)dm->sr;
        h->pitch_sweep = 3.14159f * 100.0f / (float)dm->sr; /* sweep up */
        break;
    case DRUM_SD:
        h->len = (uint32_t)(0.2f * dm->sr);
        h->pitch_inc = 2.0f * 3.14159f * 180.0f / (float)dm->sr;
        break;
    case DRUM_LT:
        h->len = (uint32_t)(0.25f * dm->sr);
        h->pitch_inc = 2.0f * 3.14159f * 100.0f / (float)dm->sr;
        break;
    case DRUM_MT:
        h->len = (uint32_t)(0.2f * dm->sr);
        h->pitch_inc = 2.0f * 3.14159f * 150.0f / (float)dm->sr;
        break;
    case DRUM_HT:
        h->len = (uint32_t)(0.15f * dm->sr);
        h->pitch_inc = 2.0f * 3.14159f * 220.0f / (float)dm->sr;
        break;
    case DRUM_CP:
        h->len = (uint32_t)(0.15f * dm->sr);
        break;
    case DRUM_RS:
        h->len = (uint32_t)(0.05f * dm->sr);
        h->pitch_inc = 2.0f * 3.14159f * 800.0f / (float)dm->sr;
        break;
    case DRUM_CB:
        h->len = (uint32_t)(0.15f * dm->sr);
        h->pitch_inc = 2.0f * 3.14159f * 600.0f / (float)dm->sr;
        break;
    case DRUM_CY:
        h->len = (uint32_t)(0.8f * dm->sr);
        h->pitch_inc = 2.0f * 3.14159f * 3500.0f / (float)dm->sr;
        break;
    case DRUM_OH:
        h->len = (uint32_t)(0.3f * dm->sr);
        h->pitch_inc = 2.0f * 3.14159f * 3500.0f / (float)dm->sr;
        break;
    case DRUM_CH:
        h->len = (uint32_t)(0.05f * dm->sr);
        h->pitch_inc = 2.0f * 3.14159f * 3500.0f / (float)dm->sr;
        break;
    default:
        h->len = (uint32_t)(0.1f * dm->sr);
        break;
    }
    h->env_step = 1.0f / (float)h->len;
}

void wb_drum_machine_note(void *inst, int note, int vel) {
    wb_drum_machine_inst *dm = (wb_drum_machine_inst *)inst;
    if (!dm || vel == 0) return;
    float v = (float)vel / 127.0f;
    /* Map MIDI notes to drum voices */
    switch (note) {
    case 36: trigger_drum(dm, DRUM_BD, v); break;
    case 38: trigger_drum(dm, DRUM_SD, v); break;
    case 41: trigger_drum(dm, DRUM_LT, v); break;
    case 45: trigger_drum(dm, DRUM_MT, v); break;
    case 50: trigger_drum(dm, DRUM_HT, v); break;
    case 39: trigger_drum(dm, DRUM_CP, v); break;
    case 37: trigger_drum(dm, DRUM_RS, v); break;
    case 56: trigger_drum(dm, DRUM_CB, v); break;
    case 49: trigger_drum(dm, DRUM_CY, v); break;
    case 46: trigger_drum(dm, DRUM_OH, v); break;
    case 42: trigger_drum(dm, DRUM_CH, v); break;
    default: break;
    }
}

/* Exponential decay envelope */
static inline float env_exp(float phase, float decay) {
    return expf(-phase * decay);
}

/* Process one hit, return mono sample */
static float process_hit(drum_hit_t *h) {
    if (!h->active) return 0.0f;

    float env = env_exp(h->env_phase, 5.0f);
    float sample = 0.0f;

    switch (h->voice) {
    case DRUM_BD: {
        /* Sine with pitch sweep from high to low */
        float sweep_inc = h->pitch_inc + h->pitch_sweep * (1.0f - h->env_phase);
        h->pitch += sweep_inc;
        if (h->pitch > 2.0f * 3.14159f) h->pitch -= 2.0f * 3.14159f;
        sample = sinf(h->pitch) * env;
        break;
    }
    case DRUM_SD: {
        /* Triangle tone + noise */
        h->pitch += h->pitch_inc;
        if (h->pitch > 2.0f * 3.14159f) h->pitch -= 2.0f * 3.14159f;
        float tone = sinf(h->pitch);
        float noise = rng_next();
        sample = (tone * 0.6f + noise * 0.4f) * env;
        break;
    }
    case DRUM_LT:
    case DRUM_MT:
    case DRUM_HT: {
        /* Tom: sine with pitch sweep */
        float sweep = h->pitch_inc * (1.0f + 0.5f * (1.0f - h->env_phase));
        h->pitch += sweep;
        if (h->pitch > 2.0f * 3.14159f) h->pitch -= 2.0f * 3.14159f;
        sample = sinf(h->pitch) * env;
        break;
    }
    case DRUM_CP: {
        /* Clap: multiple noise bursts */
        float burst = (h->env_phase < 0.3f) ? 1.0f : ((h->env_phase < 0.5f) ? 0.7f : ((h->env_phase < 0.7f) ? 0.5f : 0.3f));
        sample = rng_next() * env * burst;
        break;
    }
    case DRUM_RS: {
        /* Rimshot: triangle + noise click */
        h->pitch += h->pitch_inc;
        if (h->pitch > 2.0f * 3.14159f) h->pitch -= 2.0f * 3.14159f;
        float tone = sinf(h->pitch);
        float click = rng_next() * (h->env_phase < 0.1f ? 1.0f : 0.0f);
        sample = (tone * 0.7f + click * 0.3f) * env;
        break;
    }
    case DRUM_CB: {
        /* Cowbell: two square-like tones */
        h->pitch += h->pitch_inc;
        if (h->pitch > 2.0f * 3.14159f) h->pitch -= 2.0f * 3.14159f;
        float s1 = sinf(h->pitch) > 0 ? 1.0f : -1.0f;
        float s2 = sinf(h->pitch * 1.333f) > 0 ? 1.0f : -1.0f;
        sample = (s1 + s2) * 0.5f * env;
        break;
    }
    case DRUM_CY:
    case DRUM_OH:
    case DRUM_CH: {
        /* Cymbal/Hi-hat: metallic noise (multiple inharmonic sines + noise) */
        h->pitch += h->pitch_inc;
        if (h->pitch > 2.0f * 3.14159f) h->pitch -= 2.0f * 3.14159f;
        float s1 = sinf(h->pitch);
        float s2 = sinf(h->pitch * 1.18f);
        float s3 = sinf(h->pitch * 1.42f);
        float noise = rng_next() * 0.3f;
        sample = (s1 + s2 + s3 + noise) * 0.33f * env;
        break;
    }
    default:
        break;
    }

    /* Advance envelope */
    h->env_phase += h->env_step;
    if (h->env_phase >= 1.0f) h->active = 0;

    return sample * h->amp;
}

void wb_drum_machine_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_drum_machine_inst *dm = (wb_drum_machine_inst *)inst;
    if (!dm) return;

    memset(L, 0, n * sizeof(wb_sample));
    memset(R, 0, n * sizeof(wb_sample));

    for (uint32_t i = 0; i < n; i++) {
        float sample = 0.0f;
        for (int h = 0; h < MAX_HITS; h++) {
            if (dm->hits[h].active) {
                sample += process_hit(&dm->hits[h]);
            }
        }
        sample *= dm->volume;
        L[i] = sample;
        R[i] = sample;
    }
}
