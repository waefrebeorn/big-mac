/* wb_wavetable.c — wavetable synthesizer (Serum/Vital style).
 *
 * R078: Wavetable synthesis with morphing, unison, filtering.
 *
 * Each table is a single-cycle waveform (2048 samples).
 * Position morphs between adjacent tables.
 * Mipmapped for anti-aliasing.
 * Pure C11. */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wbus.h"

#define WT_TABLE_SIZE 2048
#define WT_MAX_TABLES 64
#define WT_MAX_VOICES 16
#define WT_MAX_UNISON 8

typedef enum {
    WT_PRESET_SINE = 0,
    WT_PRESET_SAW,
    WT_PRESET_SQUARE,
    WT_PRESET_TRIANGLE,
    WT_PRESET_PULSE_25,
    WT_PRESET_PULSE_10,
    WT_PRESET_ORGAN,
    WT_PRESET_BRASS,
    WT_PRESET_STRING,
    WT_PRESET_VOCAL,
    WT_PRESET_METAL,
    WT_PRESET_BELL,
    WT_PRESET_COUNT
} wt_preset_t;

/* struct wb_wavetable defined in wbus.h */

/* ---- single-cycle waveform generators ---- */

static void gen_sine(float *dst, int n) {
    for (int i = 0; i < n; i++)
        dst[i] = sinf(2.0f * M_PI * (float)i / (float)n);
}

static void gen_saw(float *dst, int n) {
    for (int i = 0; i < n; i++)
        dst[i] = 2.0f * (float)i / (float)n - 1.0f;
}

static void gen_square(float *dst, int n) {
    for (int i = 0; i < n; i++)
        dst[i] = (i < n / 2) ? 1.0f : -1.0f;
}

static void gen_triangle(float *dst, int n) {
    for (int i = 0; i < n; i++) {
        float x = (float)i / (float)n;
        dst[i] = 4.0f * fabsf(x - 0.5f) - 1.0f;
    }
}

static void gen_pulse(float *dst, int n, float width) {
    for (int i = 0; i < n; i++)
        dst[i] = ((float)i / (float)n < width) ? 1.0f : -1.0f;
}

static void gen_organ(float *dst, int n) {
    /* Odd harmonics with rolloff */
    memset(dst, 0, n * sizeof(float));
    for (int h = 1; h <= 7; h += 2) {
        float amp = 1.0f / (float)(h * h);
        for (int i = 0; i < n; i++)
            dst[i] += amp * sinf(2.0f * M_PI * (float)h * (float)i / (float)n);
    }
    /* Normalize */
    float peak = 0;
    for (int i = 0; i < n; i++) {
        float a = fabsf(dst[i]);
        if (a > peak) peak = a;
    }
    if (peak > 0) {
        float scale = 0.8f / peak;
        for (int i = 0; i < n; i++) dst[i] *= scale;
    }
}

static void gen_brass(float *dst, int n) {
    /* Brass-like: strong low harmonics */
    memset(dst, 0, n * sizeof(float));
    for (int h = 1; h <= 8; h++) {
        float amp = 1.0f / (float)h;
        for (int i = 0; i < n; i++)
            dst[i] += amp * sinf(2.0f * M_PI * (float)h * (float)i / (float)n);
    }
    float peak = 0;
    for (int i = 0; i < n; i++) {
        float a = fabsf(dst[i]);
        if (a > peak) peak = a;
    }
    if (peak > 0) {
        float scale = 0.7f / peak;
        for (int i = 0; i < n; i++) dst[i] *= scale;
    }
}

static void gen_string(float *dst, int n) {
    /* String-like: saw-ish with slight detune beating */
    memset(dst, 0, n * sizeof(float));
    for (int h = 1; h <= 12; h++) {
        float amp = 1.0f / (float)h;
        float detune = 1.0f + 0.001f * (float)(h % 2);
        for (int i = 0; i < n; i++)
            dst[i] += amp * sinf(2.0f * M_PI * (float)h * detune * (float)i / (float)n);
    }
    float peak = 0;
    for (int i = 0; i < n; i++) {
        float a = fabsf(dst[i]);
        if (a > peak) peak = a;
    }
    if (peak > 0) {
        float scale = 0.7f / peak;
        for (int i = 0; i < n; i++) dst[i] *= scale;
    }
}

static void gen_vocal(float *dst, int n) {
    /* Vocal-like: formant-shaped spectrum */
    memset(dst, 0, n * sizeof(float));
    /* Boost around harmonics 5-7 (formant region) */
    for (int h = 1; h <= 20; h++) {
        float amp = 1.0f / (float)h;
        if (h >= 4 && h <= 8) amp *= 3.0f; /* formant boost */
        for (int i = 0; i < n; i++)
            dst[i] += amp * sinf(2.0f * M_PI * (float)h * (float)i / (float)n);
    }
    float peak = 0;
    for (int i = 0; i < n; i++) {
        float a = fabsf(dst[i]);
        if (a > peak) peak = a;
    }
    if (peak > 0) {
        float scale = 0.6f / peak;
        for (int i = 0; i < n; i++) dst[i] *= scale;
    }
}

static void gen_metal(float *dst, int n) {
    /* Metallic: inharmonic partials */
    memset(dst, 0, n * sizeof(float));
    float partials[] = {1.0f, 2.7f, 5.4f, 8.1f, 10.8f, 13.5f};
    for (int p = 0; p < 6; p++) {
        float amp = 1.0f / (1.0f + (float)p);
        for (int i = 0; i < n; i++)
            dst[i] += amp * sinf(2.0f * M_PI * partials[p] * (float)i / (float)n);
    }
    float peak = 0;
    for (int i = 0; i < n; i++) {
        float a = fabsf(dst[i]);
        if (a > peak) peak = a;
    }
    if (peak > 0) {
        float scale = 0.7f / peak;
        for (int i = 0; i < n; i++) dst[i] *= scale;
    }
}

static void gen_bell(float *dst, int n) {
    /* Bell: inharmonic with fast decay of high partials */
    memset(dst, 0, n * sizeof(float));
    float partials[] = {1.0f, 2.0f, 3.0f, 4.2f, 5.4f, 6.8f};
    float amps[] = {1.0f, 0.7f, 0.5f, 0.3f, 0.2f, 0.1f};
    for (int p = 0; p < 6; p++) {
        for (int i = 0; i < n; i++)
            dst[i] += amps[p] * sinf(2.0f * M_PI * partials[p] * (float)i / (float)n);
    }
    float peak = 0;
    for (int i = 0; i < n; i++) {
        float a = fabsf(dst[i]);
        if (a > peak) peak = a;
    }
    if (peak > 0) {
        float scale = 0.7f / peak;
        for (int i = 0; i < n; i++) dst[i] *= scale;
    }
}

/* ---- mipmap generation ---- */

static void generate_mipmap(float *dst, const float *src, int n) {
    /* Simple box-downsample by 2x */
    int half = n / 2;
    for (int i = 0; i < half; i++)
        dst[i] = (src[i * 2] + src[i * 2 + 1]) * 0.5f;
}

/* ---- public API ---- */

void *wb_wavetable_create(uint32_t sr) {
    wb_wavetable *wt = (wb_wavetable *)calloc(1, sizeof(*wt));
    if (!wt) return NULL;
    wt->sr = sr;
    wt->table_size = WT_TABLE_SIZE;
    wt->position = 0;
    wt->interp_mode = 1;
    wt->unison_voices = 1;
    wt->unison_spread = 0;
    wt->filter_cutoff = (float)sr * 0.5f; /* open */
    wt->filter_resonance = 0;
    wt->filter_z1 = 0;
    wt->active_voices = 0;
    return wt;
}

void wb_wavetable_destroy(void *inst) {
    wb_wavetable *wt = (wb_wavetable *)inst;
    if (!wt) return;
    for (int t = 0; t < wt->table_count; t++) {
        free(wt->tables[t]);
        for (int m = 0; m < wt->mip_levels[t]; m++)
            free(wt->mipmaps[t][m]);
    }
    free(wt);
}

int wb_wavetable_generate_wavetable(void *inst, int table_count, int preset) {
    wb_wavetable *wt = (wb_wavetable *)inst;
    if (!wt || table_count < 1 || table_count > WT_MAX_TABLES) return -1;
    /* Free old tables */
    for (int t = 0; t < wt->table_count; t++) {
        free(wt->tables[t]);
        for (int m = 0; m < wt->mip_levels[m]; m++)
            free(wt->mipmaps[t][m]);
        wt->tables[t] = NULL;
        wt->mip_levels[t] = 0;
    }
    wt->table_count = table_count;

    /* Generate tables: interpolate from preset to preset+1 across tables */
    int p0 = preset % WT_PRESET_COUNT;
    int p1 = (preset + 1) % WT_PRESET_COUNT;

    for (int t = 0; t < table_count; t++) {
        wt->tables[t] = (float *)calloc(WT_TABLE_SIZE, sizeof(float));
        if (!wt->tables[t]) return -1;

        float *tmp0 = (float *)calloc(WT_TABLE_SIZE, sizeof(float));
        float *tmp1 = (float *)calloc(WT_TABLE_SIZE, sizeof(float));
        if (!tmp0 || !tmp1) { free(tmp0); free(tmp1); return -1; }

        /* Generate both presets */
        switch (p0) {
            case WT_PRESET_SINE: gen_sine(tmp0, WT_TABLE_SIZE); break;
            case WT_PRESET_SAW: gen_saw(tmp0, WT_TABLE_SIZE); break;
            case WT_PRESET_SQUARE: gen_square(tmp0, WT_TABLE_SIZE); break;
            case WT_PRESET_TRIANGLE: gen_triangle(tmp0, WT_TABLE_SIZE); break;
            case WT_PRESET_PULSE_25: gen_pulse(tmp0, WT_TABLE_SIZE, 0.25f); break;
            case WT_PRESET_PULSE_10: gen_pulse(tmp0, WT_TABLE_SIZE, 0.10f); break;
            case WT_PRESET_ORGAN: gen_organ(tmp0, WT_TABLE_SIZE); break;
            case WT_PRESET_BRASS: gen_brass(tmp0, WT_TABLE_SIZE); break;
            case WT_PRESET_STRING: gen_string(tmp0, WT_TABLE_SIZE); break;
            case WT_PRESET_VOCAL: gen_vocal(tmp0, WT_TABLE_SIZE); break;
            case WT_PRESET_METAL: gen_metal(tmp0, WT_TABLE_SIZE); break;
            case WT_PRESET_BELL: gen_bell(tmp0, WT_TABLE_SIZE); break;
        }
        switch (p1) {
            case WT_PRESET_SINE: gen_sine(tmp1, WT_TABLE_SIZE); break;
            case WT_PRESET_SAW: gen_saw(tmp1, WT_TABLE_SIZE); break;
            case WT_PRESET_SQUARE: gen_square(tmp1, WT_TABLE_SIZE); break;
            case WT_PRESET_TRIANGLE: gen_triangle(tmp1, WT_TABLE_SIZE); break;
            case WT_PRESET_PULSE_25: gen_pulse(tmp1, WT_TABLE_SIZE, 0.25f); break;
            case WT_PRESET_PULSE_10: gen_pulse(tmp1, WT_TABLE_SIZE, 0.10f); break;
            case WT_PRESET_ORGAN: gen_organ(tmp1, WT_TABLE_SIZE); break;
            case WT_PRESET_BRASS: gen_brass(tmp1, WT_TABLE_SIZE); break;
            case WT_PRESET_STRING: gen_string(tmp1, WT_TABLE_SIZE); break;
            case WT_PRESET_VOCAL: gen_vocal(tmp1, WT_TABLE_SIZE); break;
            case WT_PRESET_METAL: gen_metal(tmp1, WT_TABLE_SIZE); break;
            case WT_PRESET_BELL: gen_bell(tmp1, WT_TABLE_SIZE); break;
        }

        /* Interpolate */
        float frac = (float)t / (float)(table_count > 1 ? table_count - 1 : 1);
        for (int i = 0; i < WT_TABLE_SIZE; i++)
            wt->tables[t][i] = tmp0[i] * (1.0f - frac) + tmp1[i] * frac;
        free(tmp0);
        free(tmp1);

        /* Generate mipmaps */
        int ms = WT_TABLE_SIZE;
        float *src = wt->tables[t];
        for (int lvl = 0; lvl < 8 && ms >= 64; lvl++) {
            wt->mipmaps[t][lvl] = (float *)calloc(ms / 2, sizeof(float));
            if (!wt->mipmaps[t][lvl]) break;
            generate_mipmap(wt->mipmaps[t][lvl], src, ms);
            src = wt->mipmaps[t][lvl];
            ms /= 2;
            wt->mip_levels[t] = lvl + 1;
        }
    }
    return 0;
}

void wb_wavetable_set_position(void *inst, float pos) {
    wb_wavetable *wt = (wb_wavetable *)inst;
    if (!wt) return;
    wt->position = pos < 0 ? 0 : (pos > 1 ? 1 : pos);
}

void wb_wavetable_set_interpolation(void *inst, int mode) {
    wb_wavetable *wt = (wb_wavetable *)inst;
    if (!wt) return;
    wt->interp_mode = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
}

void wb_wavetable_set_unison(void *inst, int voices, float spread) {
    wb_wavetable *wt = (wb_wavetable *)inst;
    if (!wt) return;
    wt->unison_voices = voices < 1 ? 1 : (voices > WT_MAX_UNISON ? WT_MAX_UNISON : voices);
    wt->unison_spread = spread < 0 ? 0 : (spread > 1 ? 1 : spread);
}

void wb_wavetable_set_filter(void *inst, float cutoff, float resonance) {
    wb_wavetable *wt = (wb_wavetable *)inst;
    if (!wt) return;
    wt->filter_cutoff = cutoff > 0 ? cutoff : 100;
    wt->filter_resonance = resonance < 0 ? 0 : (resonance > 1 ? 1 : resonance);
}

void wb_wavetable_note(void *inst, int note, int vel) {
    wb_wavetable *wt = (wb_wavetable *)inst;
    if (!wt) return;
    /* Find free voice (round-robin) */
    int voice = -1;
    for (int i = 0; i < WT_MAX_VOICES; i++) {
        if (!wt->voices[i].active) { voice = i; break; }
    }
    if (voice < 0) voice = 0; /* steal voice 0 */
    struct wt_voice *v = &wt->voices[voice];
    v->midi_note = note;
    v->velocity = (float)vel / 127.0f;
    v->phase_inc = 440.0f * powf(2.0f, (note - 69) / 12.0f) / (float)wt->sr;
    v->phase = 0;
    v->active = 1;
    if (voice >= wt->active_voices) wt->active_voices = voice + 1;
}

/* Sample from a table at fractional position with chosen interpolation */
static float sample_table(const float *table, int size, float pos, int mode) {
    if (pos >= (float)pos) pos -= (float)(int)pos; /* wrap */
    if (pos < 0) pos += 1.0f;
    float idx = pos * (float)size;
    int i0 = (int)idx;
    float frac = idx - (float)i0;
    int i1 = (i0 + 1) % size;
    int im1 = (i0 - 1 + size) % size;
    int i2 = (i0 + 2) % size;

    switch (mode) {
    case 0: /* nearest */
        return (frac < 0.5f) ? table[i0] : table[i1];
    case 2: { /* cubic (Catmull-Rom) */
        float y0 = table[im1], y1 = table[i0], y2 = table[i1], y3 = table[i2];
        return 0.5f * ((2.0f * y1) + (-y0 + y2) * frac
                 + (2.0f*y0 - 5.0f*y1 + 4.0f*y2 - y3) * frac*frac
                 + (-y0 + 3.0f*y1 - 3.0f*y2 + y3) * frac*frac*frac);
    }
    default: /* linear */
        return table[i0] * (1.0f - frac) + table[i1] * frac;
    }
}

void wb_wavetable_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_wavetable *wt = (wb_wavetable *)inst;
    if (!wt || !L || !R || wt->table_count == 0) {
        if (L) memset(L, 0, n * sizeof(wb_sample));
        if (R) memset(R, 0, n * sizeof(wb_sample));
        return;
    }

    memset(L, 0, n * sizeof(wb_sample));
    memset(R, 0, n * sizeof(wb_sample));

    for (int v = 0; v < wt->active_voices; v++) {
        struct wt_voice *voice = &wt->voices[v];
        if (!voice->active) continue;

        for (uint32_t i = 0; i < n; i++) {
            /* Determine which tables to morph between */
            float table_pos = wt->position * (float)(wt->table_count - 1);
            int t0 = (int)table_pos;
            int t1 = t0 + 1;
            if (t1 >= wt->table_count) t1 = wt->table_count - 1;
            float tfrac = table_pos - (float)t0;

            /* Sample from both tables */
            float s0 = sample_table(wt->tables[t0], wt->table_size, voice->phase, wt->interp_mode);
            float s1 = sample_table(wt->tables[t1], wt->table_size, voice->phase, wt->interp_mode);
            float sample = s0 * (1.0f - tfrac) + s1 * tfrac;

            /* Unison: add detuned copies */
            float mono = sample;
            if (wt->unison_voices > 1) {
                float sum = sample;
                for (int u = 1; u < wt->unison_voices; u++) {
                    float detune = ((float)u / (float)(wt->unison_voices - 1) - 0.5f) * wt->unison_spread * 0.02f;
                    float detuned_inc = voice->phase_inc * (1.0f + detune);
                    float detuned_phase = voice->phase * (1.0f + detune);
                    float ds0 = sample_table(wt->tables[t0], wt->table_size, detuned_phase, wt->interp_mode);
                    float ds1 = sample_table(wt->tables[t1], wt->table_size, detuned_phase, wt->interp_mode);
                    sum += ds0 * (1.0f - tfrac) + ds1 * tfrac;
                }
                sample = sum / (float)wt->unison_voices;
            }

            /* Apply velocity */
            sample *= voice->velocity;

            /* Simple one-pole lowpass filter */
            float cutoff_norm = wt->filter_cutoff / (float)wt->sr;
            if (cutoff_norm > 0.5f) cutoff_norm = 0.5f;
            float a = cutoff_norm * 2.0f; /* crude approximation */
            wt->filter_z1 += a * (sample - wt->filter_z1);
            sample = wt->filter_z1;

            /* Stereo: pan unison voices */
            float pan = 0;
            if (wt->unison_voices > 1) {
                pan = ((float)v / (float)(wt->active_voices - 1) - 0.5f) * 0.8f;
            }
            float left_gain = (pan <= 0) ? 1.0f : (1.0f - pan);
            float right_gain = (pan >= 0) ? 1.0f : (1.0f + pan);
            L[i] += sample * left_gain;
            R[i] += sample * right_gain;

            /* Advance phase */
            voice->phase += voice->phase_inc;
            if (voice->phase >= 1.0f) voice->phase -= 1.0f;
        }
    }

    /* Normalize to prevent clipping */
    float peak = 0;
    for (uint32_t i = 0; i < n; i++) {
        float a = fabsf(L[i]);
        if (a > peak) peak = a;
        a = fabsf(R[i]);
        if (a > peak) peak = a;
    }
    if (peak > 1.0f) {
        float scale = 0.9f / peak;
        for (uint32_t i = 0; i < n; i++) {
            L[i] *= scale;
            R[i] *= scale;
        }
    }
}
