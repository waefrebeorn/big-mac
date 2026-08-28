/* wb_meme_sounds.c — meme/YTP sound effect generator.
 *
 * Generates iconic meme sounds procedurally (no samples needed):
 *   - Vine boom
 *   - Bass boost drop
 *   - Earrape flash
 *   - "Oh no" / sad violin
 *   - "Bruh" sound
 *   - "Yeet" whoosh
 *   - Windows XP error
 *   - Discord ping
 *   - TikTok transition whoosh
 *   - "Rizz" sound
 *   - Crickets / silence
 *   - "Nice" (CS:GO)
 *   - "Oof" (Roblox death)
 *   - Wilhelm scream placeholder
 *   - "It's morbin time"
 *
 * Pure C11, procedural synthesis. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef enum {
    MEME_VINE_BOOM = 0,
    MEME_BASS_DROP,
    MEME_EARRAPE,
    MEME_SAD_VIOLIN,
    MEME_BRUH,
    MEME_YEET,
    MEME_WINDOWS_ERROR,
    MEME_DISCORD_PING,
    MEME_TIKTOK_WHOOSH,
    MEME_NICE,
    MEME_ROBLOX_OOF,
    MEME_CRICKETS,
    MEME_RIZZ,
    MEME_MORBIN,
    MEME_WILHELM,
    MEME_COUNT
} meme_sound_t;

typedef struct {
    meme_sound_t type;
    float *buf;
    int     len;
    int     pos;
    int     active;
} meme_sound_instance;

#define MAX_MEME_SOUNDS 16

typedef struct {
    uint32_t sr;
    meme_sound_instance sounds[MAX_MEME_SOUNDS];
} wb_meme_sounds_inst;

static unsigned int meme_rng = 0xDEADBEEFu;

static float meme_rng_next(void) {
    meme_rng ^= meme_rng << 13;
    meme_rng ^= meme_rng >> 17;
    meme_rng ^= meme_rng << 5;
    return (float)((double)(meme_rng & 0xFFFF) / 32768.0 - 1.0);
}

void *wb_meme_sounds_create(uint32_t sr) {
    wb_meme_sounds_inst *m = (wb_meme_sounds_inst *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->sr = sr;
    return m;
}

void wb_meme_sounds_destroy(void *inst) {
    wb_meme_sounds_inst *m = (wb_meme_sounds_inst *)inst;
    if (!m) return;
    for (int i = 0; i < MAX_MEME_SOUNDS; i++) {
        if (m->sounds[i].buf) free(m->sounds[i].buf);
    }
    free(m);
}

/* Generate a meme sound into a buffer */
static float* generate_meme_sound(meme_sound_t type, uint32_t sr, int *out_len) {
    int len = 0;
    float *buf = NULL;

    switch (type) {
    case MEME_VINE_BOOM: {
        /* Deep impact: 80Hz sine with fast decay + noise burst */
        len = (int)(0.3f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            float env = expf(-t * 15.0f);
            float sine = sinf(2.0f * 3.14159f * 80.0f * t);
            float noise = meme_rng_next() * 0.3f * expf(-t * 50.0f);
            buf[i] = (sine + noise) * env;
        }
        break;
    }
    case MEME_BASS_DROP: {
        /* Sub-bass sweep from 150Hz to 40Hz */
        len = (int)(0.5f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            float env = expf(-t * 4.0f);
            float freq = 150.0f * expf(-t * 3.0f) + 40.0f;
            float phase = 2.0f * 3.14159f * freq * t;
            buf[i] = sinf(phase) * env;
        }
        break;
    }
    case MEME_EARRAPE: {
        /* Sine wave at max amplitude with harsh clipping */
        len = (int)(0.2f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            float s = sinf(2.0f * 3.14159f * 1000.0f * t);
            /* Hard clip to square */
            s = s > 0 ? 1.0f : -1.0f;
            buf[i] = s * expf(-t * 10.0f);
        }
        break;
    }
    case MEME_SAD_VIOLIN: {
        /* Descending minor melody (the "oh no" tune) */
        len = (int)(1.5f * sr);
        buf = (float *)calloc(len, sizeof(float));
        /* Notes: A4, Eb4, C4, A3 (descending minor) */
        float freqs[] = {440.0f, 311.0f, 261.6f, 220.0f};
        float durs[] = {0.35f, 0.35f, 0.35f, 0.45f};
        int pos = 0;
        for (int n = 0; n < 4; n++) {
            int note_len = (int)(durs[n] * sr);
            for (int i = 0; i < note_len && pos < len; i++, pos++) {
                float t = (float)i / (float)sr;
                float env = expf(-t * 3.0f);
                /* Sawtooth-like (vowel formant) */
                float s = 0;
                for (int h = 1; h <= 4; h++) {
                    s += sinf(2.0f * 3.14159f * freqs[n] * (float)h * t) / (float)h;
                }
                buf[pos] = s * env * 0.3f;
            }
        }
        break;
    }
    case MEME_BRUH: {
        /* Short descending "bruh" — formant-like */
        len = (int)(0.3f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            float env = expf(-t * 8.0f);
            float freq = 300.0f - t * 200.0f; /* descending */
            if (freq < 80) freq = 80;
            float phase = 2.0f * 3.14159f * freq * t;
            /* Formant shape: emphasize ~500Hz */
            float s = sinf(phase) * 0.7f + sinf(phase * 2.0f) * 0.3f;
            buf[i] = s * env;
        }
        break;
    }
    case MEME_YEET: {
        /* Rising whoosh + impact */
        len = (int)(0.4f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            /* Rising noise */
            float noise = meme_rng_next();
            float freq = 200.0f + t * 3000.0f;
            float s = sinf(2.0f * 3.14159f * freq * t) * 0.3f + noise * 0.7f;
            float env = (t < 0.2f) ? t * 5.0f : expf(-(t - 0.2f) * 10.0f);
            buf[i] = s * env;
        }
        break;
    }
    case MEME_WINDOWS_ERROR: {
        /* Windows XP error chord: two dissonant tones */
        len = (int)(0.4f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            float env = expf(-t * 5.0f);
            float s = sinf(2.0f * 3.14159f * 440.0f * t) * 0.5f +
                      sinf(2.0f * 3.14159f * 466.0f * t) * 0.5f;
            buf[i] = s * env;
        }
        break;
    }
    case MEME_DISCORD_PING: {
        /* Two-tone ping: C5 then E5 */
        len = (int)(0.3f * sr);
        buf = (float *)calloc(len, sizeof(float));
        int half = len / 2;
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            float env = expf(-t * 8.0f);
            float freq = (i < half) ? 523.25f : 659.25f;
            buf[i] = sinf(2.0f * 3.14159f * freq * t) * env;
        }
        break;
    }
    case MEME_TIKTOK_WHOOSH: {
        /* Bandpass-filtered noise sweep */
        len = (int)(0.2f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            float noise = meme_rng_next();
            float env = sinf(3.14159f * t / 0.2f); /* bell shape */
            buf[i] = noise * env * 0.5f;
        }
        break;
    }
    case MEME_NICE: {
        /* CS:GO "Nice" — short rising tone */
        len = (int)(0.2f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            float env = expf(-t * 10.0f);
            float freq = 800.0f + t * 400.0f;
            buf[i] = sinf(2.0f * 3.14159f * freq * t) * env;
        }
        break;
    }
    case MEME_ROBLOX_OOF: {
        /* Roblox "Oof" — short ascending then descending */
        len = (int)(0.3f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            float env = expf(-t * 10.0f);
            float freq = (t < 0.1f) ? 400.0f + t * 2000.0f : 600.0f - (t - 0.1f) * 200.0f;
            if (freq < 200) freq = 200;
            buf[i] = sinf(2.0f * 3.14159f * freq * t) * env;
        }
        break;
    }
    case MEME_RIZZ: {
        /* Smooth jazz flute-like (the "rizz" sound) */
        len = (int)(1.0f * sr);
        buf = (float *)calloc(len, sizeof(float));
        /* Descending pentatonic */
        float freqs[] = {523.0f, 440.0f, 392.0f, 349.0f, 294.0f};
        float durs[] = {0.2f, 0.2f, 0.2f, 0.2f, 0.2f};
        int pos = 0;
        for (int n = 0; n < 5; n++) {
            int note_len = (int)(durs[n] * sr);
            for (int i = 0; i < note_len && pos < len; i++, pos++) {
                float t = (float)i / (float)sr;
                float env = expf(-t * 4.0f);
                float s = sinf(2.0f * 3.14159f * freqs[n] * t);
                buf[pos] = s * env * 0.5f;
            }
        }
        break;
    }
    case MEME_MORBIN: {
        /* "It's morbin time!" — dramatic rising then drop */
        len = (int)(0.5f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            float env = expf(-t * 3.0f);
            float freq;
            if (t < 0.25f) freq = 200.0f + t * 1600.0f; /* rising */
            else if (t < 0.35f) freq = 600.0f; /* hold */
            else freq = 600.0f - (t - 0.35f) * 1000.0f; /* drop */
            if (freq < 100) freq = 100;
            buf[i] = sinf(2.0f * 3.14159f * freq * t) * env;
        }
        break;
    }
    case MEME_WILHELM: {
        /* Wilhelm scream placeholder: rising then falling scream */
        len = (int)(1.0f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            float env = expf(-t * 2.0f);
            float freq;
            if (t < 0.3f) freq = 400.0f + t * 2000.0f;
            else if (t < 0.5f) freq = 1000.0f;
            else freq = 1000.0f - (t - 0.5f) * 800.0f;
            if (freq < 200) freq = 200;
            float s = sinf(2.0f * 3.14159f * freq * t);
            /* Add vibrato */
            s *= 1.0f + 0.1f * sinf(2.0f * 3.14159f * 5.0f * t);
            buf[i] = s * env * 0.4f;
        }
        break;
    }
    case MEME_CRICKETS: {
        /* Crickets: high-frequency chirps with silence gaps */
        len = (int)(2.0f * sr);
        buf = (float *)calloc(len, sizeof(float));
        for (int i = 0; i < len; i++) {
            float t = (float)i / (float)sr;
            /* Chirp pattern: 4 chirps per second */
            float chirp_phase = fmodf(t * 4.0f, 1.0f);
            float chirp_env = (chirp_phase < 0.3f) ? 1.0f : 0.0f;
            float s = sinf(2.0f * 3.14159f * 4000.0f * t) * chirp_env;
            buf[i] = s * 0.15f; /* quiet */
        }
        break;
    }
    default:
        len = (int)(0.1f * sr);
        buf = (float *)calloc(len, sizeof(float));
        break;
    }

    *out_len = len;
    return buf;
}

void wb_meme_sounds_play(void *inst, meme_sound_t type) {
    wb_meme_sounds_inst *m = (wb_meme_sounds_inst *)inst;
    if (!m) return;

    int slot = -1;
    for (int i = 0; i < MAX_MEME_SOUNDS; i++) {
        if (!m->sounds[i].active) { slot = i; break; }
    }
    if (slot < 0) slot = 0;

    if (m->sounds[slot].buf) free(m->sounds[slot].buf);

    int len;
    float *buf = generate_meme_sound(type, m->sr, &len);
    m->sounds[slot].buf = buf;
    m->sounds[slot].len = len;
    m->sounds[slot].pos = 0;
    m->sounds[slot].type = type;
    m->sounds[slot].active = 1;
}

void wb_meme_sounds_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_meme_sounds_inst *m = (wb_meme_sounds_inst *)inst;
    if (!m) return;

    for (uint32_t i = 0; i < n; i++) {
        float sample = 0.0f;
        for (int s = 0; s < MAX_MEME_SOUNDS; s++) {
            if (m->sounds[s].active && m->sounds[s].pos < m->sounds[s].len) {
                sample += m->sounds[s].buf[m->sounds[s].pos++];
                if (m->sounds[s].pos >= m->sounds[s].len) {
                    m->sounds[s].active = 0;
                }
            }
        }
        L[i] = sample;
        R[i] = sample;
    }
}
