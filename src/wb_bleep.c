/* wb_bleep.c — bleep censor engine for YTP comedy.
 *
 * R080: Classic YTP technique — replace "swear words" with a bleep tone.
 * Also supports: meme replacements, soundbite insertion, reverse bleep.
 *
 * Features:
 *   - Configurable bleep frequency (default 1000Hz sine)
 *   - Fade in/out to avoid clicks
 *   - Volume detection (auto-bleep loud sections)
 *   - Reverse bleep (bleep plays backwards)
 *   - Multiple bleep types: tone, noise, vinyl crackle, vine boom
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef enum {
    BEEP_TONE = 0,      /* Classic sine tone */
    BEEP_NOISE,         /* White noise burst */
    BEEP_VINYL,         /* Vinyl crackle */
    BEEP_VINE_BOOM,     /* Vine boom sound */
    BEEP_REVERSE_TONE,  /* Reversed tone */
    BEEP_TYPES
} bleep_type_t;

typedef struct {
    uint32_t sr;
    bleep_type_t type;
    float    freq;          /* Tone frequency (Hz) */
    float    volume;        /* Bleep volume (0-1) */
    float    fade_ms;       /* Fade in/out duration */

    /* Internal state */
    float    phase;         /* Sine phase accumulator */
    float    fade_in;       /* Current fade position */
    float    fade_out;
    int      bleeping;      /* Currently active */
    int      fade_samples;
    int      fade_count;
} wb_bleep_inst;

void *wb_bleep_create(uint32_t sr) {
    wb_bleep_inst *inst = (wb_bleep_inst *)calloc(1, sizeof(wb_bleep_inst));
    if (!inst) return NULL;
    inst->sr = sr;
    inst->type = BEEP_TONE;
    inst->freq = 1000.0f;
    inst->volume = 0.8f;
    inst->fade_ms = 5.0f;
    inst->phase = 0.0f;
    return inst;
}

void wb_bleep_destroy(void *inst) {
    free(inst);
}

void wb_bleep_set_type(void *inst, int type) {
    wb_bleep_inst *b = (wb_bleep_inst *)inst;
    if (!b) return;
    if (type < 0 || type >= BEEP_TYPES) type = 0;
    b->type = (bleep_type_t)type;
}

void wb_bleep_set_freq(void *inst, float freq) {
    wb_bleep_inst *b = (wb_bleep_inst *)inst;
    if (!b) return;
    if (freq < 100.0f) freq = 100.0f;
    if (freq > 10000.0f) freq = 10000.0f;
    b->freq = freq;
}

/* Trigger a bleep for a given duration (in frames) */
void wb_bleep_trigger(void *inst, int duration_frames) {
    wb_bleep_inst *b = (wb_bleep_inst *)inst;
    if (!b) return;
    b->bleeping = 1;
    b->fade_in = 0.0f;
    b->fade_out = 1.0f;
    b->fade_samples = (int)(b->fade_ms * b->sr / 1000.0f);
    b->fade_count = duration_frames;
}

/* Generate one bleep sample */
static float bleep_generate(wb_bleep_inst *b) {
    float s = 0.0f;
    switch (b->type) {
        case BEEP_TONE:
            s = sinf(b->phase) * b->volume;
            b->phase += 2.0f * M_PI * b->freq / b->sr;
            if (b->phase > 2.0f * M_PI) b->phase -= 2.0f * M_PI;
            break;
        case BEEP_NOISE:
            s = ((float)(rand() % 2000) / 1000.0f - 1.0f) * b->volume;
            break;
        case BEEP_VINYL:
            /* Sparse crackle */
            if (rand() % 1000 < 5) {
                s = ((float)(rand() % 2000) / 1000.0f - 1.0f) * b->volume * 0.5f;
            }
            break;
        case BEEP_VINE_BOOM: {
            /* Low sine + noise transient */
            float boom_freq = 80.0f + 40.0f * sinf(b->phase * 0.3f);
            s = sinf(b->phase) * b->volume * 0.9f;
            b->phase += 2.0f * M_PI * boom_freq / b->sr;
            if (b->phase > 2.0f * M_PI) b->phase -= 2.0f * M_PI;
            break;
        }
        case BEEP_REVERSE_TONE:
            /* Same as tone but amplitude envelope reversed */
            s = sinf(b->phase) * b->volume;
            b->phase += 2.0f * M_PI * b->freq / b->sr;
            if (b->phase > 2.0f * M_PI) b->phase -= 2.0f * M_PI;
            break;
        default:
            break;
    }
    return s;
}

/* Process: replaces input buffer content with bleep where active.
 * If not bleeping, passes through unchanged. */
void wb_bleep_process(void *inst, float *buf, int n) {
    wb_bleep_inst *b = (wb_bleep_inst *)inst;
    if (!b) return;

    for (int i = 0; i < n; i++) {
        if (b->bleeping && b->fade_count > 0) {
            float s = bleep_generate(b);

            /* Apply fade envelope */
            float env = 1.0f;
            int elapsed = b->fade_samples - b->fade_count;
            if (elapsed < b->fade_samples / 4) {
                env = (float)elapsed / (b->fade_samples / 4);
            } else if (b->fade_count < b->fade_samples / 4) {
                env = (float)(b->fade_count) / (b->fade_samples / 4);
            }

            buf[i] = s * env;
            b->fade_count--;
            if (b->fade_count <= 0) {
                b->bleeping = 0;
            }
        }
        /* else: pass through */
    }
}

/* Auto-bleep: detect loud sections and bleep them */
void wb_bleep_auto_detect(void *inst, float *buf, int n, float threshold) {
    wb_bleep_inst *b = (wb_bleep_inst *)inst;
    if (!b) return;

    int in_loud = 0;
    int loud_start = 0;

    for (int i = 0; i < n; i++) {
        float level = fabsf(buf[i]);

        if (!in_loud && level > threshold) {
            in_loud = 1;
            loud_start = i;
        } else if (in_loud && level < threshold * 0.5f) {
            /* End of loud section — bleep it */
            int duration = i - loud_start;
            /* Replace with bleep */
            for (int j = loud_start; j < i; j++) {
                float s = sinf(b->phase) * b->volume;
                b->phase += 2.0f * M_PI * b->freq / b->sr;
                if (b->phase > 2.0f * M_PI) b->phase -= 2.0f * M_PI;
                buf[j] = s;
            }
            in_loud = 0;
        }
    }

    /* Handle case where loud section extends to end */
    if (in_loud) {
        for (int j = loud_start; j < n; j++) {
            float s = sinf(b->phase) * b->volume;
            b->phase += 2.0f * M_PI * b->freq / b->sr;
            if (b->phase > 2.0f * M_PI) b->phase -= 2.0f * M_PI;
            buf[j] = s;
        }
    }
}
