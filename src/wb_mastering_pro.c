/* wb_mastering_pro.c — professional mastering chain (iZotope Ozone style).
 *
 * Chain: input gain → 4-band crossover → per-band compressor → stereo widener
 *        → bass mono → limiter → output gain → loudness normalizer
 *
 * 4-band crossover: LR4 at 100 Hz, 1 kHz, 8 kHz (4th-order Linkwitz-Riley =
 *   cascaded Butterworth lowpass/highpass, 2 biquads each).
 * Per-band compression: independent threshold/ratio/attack/release per band.
 * Stereo width: M/S processing with adjustable mid/side ratio.
 * Bass mono: fold low frequencies below 120 Hz to mono.
 * Limiter: look-ahead brickwall at -1 dBTP.
 * Loudness: measure integrated LUFS, adjust gain to target.
 *
 * Pure C11, no third-party. Reuses wb_comp, wb_lufs, wb_filter primitives.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wbus.h"

/* ------------------------------------------------------------------ */
/* External primitives (declared locally — these modules are standalone
 * and not exported through wbus.h).                                  */
/* ------------------------------------------------------------------ */

/* ---- compressor (wb_comp.c) ---- */
void *wb_comp_create(uint32_t sr);
void  wb_comp_destroy(void *inst);
void  wb_comp_set(void *inst, int param, float v);
void  wb_comp_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* ---- K-weighted loudness (wb_lufs.c) ---- */
#include "wbus_lufs.h"

/* ---- true-peak limiter (wb_true_peak.c) ---- */
void *wb_true_peak_create(uint32_t sr);
void  wb_true_peak_destroy(void *inst);
void  wb_true_peak_set(void *inst, int param, float v);
void  wb_true_peak_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* ---- stereo image (wb_stereo_image.c) ---- */
void *wb_stereo_image_create(uint32_t sr);
void  wb_stereo_image_destroy(void *inst);
void  wb_stereo_image_set(void *inst, int param, float v);
void  wb_stereo_image_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* ------------------------------------------------------------------ */
/* LR4 crossover: 4th-order Linkwitz-Riley = two cascaded Butterworth
 * biquads per split frequency. For a 4-band setup we need 3 crossover
 * frequencies (100, 1000, 8000 Hz), each producing a lowpass and
 * highpass output.                                                     */
/* ------------------------------------------------------------------ */

#define NBANDS 4
#define MAX_LOOKAHEAD 512

/* A single biquad stage (reused from wb_filter primitives). */
typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} lr4_biquad;

static void lr4_biquad_lp(lr4_biquad *f, float sr, float freq) {
    /* 2nd-order Butterworth lowpass (Q=0.7071). */
    float omega = 2.0f * 3.14159265f * freq / sr;
    float cos_o = cosf(omega);
    float sin_o = sinf(omega);
    float alpha = sin_o / 1.41421356f; /* Q = 1/sqrt(2) */
    float a0 = 1.0f + alpha;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
    f->b0 = (1.0f - cos_o) / (2.0f * a0);
    f->b1 = (1.0f - cos_o) / a0;
    f->b2 = (1.0f - cos_o) / (2.0f * a0);
    f->a1 = (-2.0f * cos_o) / a0;
    f->a2 = (1.0f - alpha) / a0;
}

static void lr4_biquad_hp(lr4_biquad *f, float sr, float freq) {
    /* 2nd-order Butterworth highpass (Q=0.7071). */
    float omega = 2.0f * 3.14159265f * freq / sr;
    float cos_o = cosf(omega);
    float sin_o = sinf(omega);
    float alpha = sin_o / 1.41421356f;
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f + cos_o) / (2.0f * a0);
    f->b1 = -(1.0f + cos_o) / a0;
    f->b2 = (1.0f + cos_o) / (2.0f * a0);
    f->a1 = (-2.0f * cos_o) / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}

static inline float lr4_biquad_tick(lr4_biquad *f, float x) {
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
              - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

/* LR4 crossover filter pair: produces low and high outputs from input. */
typedef struct {
    lr4_biquad lp[2]; /* two cascaded LP biquads */
    lr4_biquad hp[2]; /* two cascaded HP biquads */
} lr4_split;

static void lr4_split_init(lr4_split *s, float sr, float freq) {
    lr4_biquad_lp(&s->lp[0], sr, freq);
    lr4_biquad_lp(&s->lp[1], sr, freq);
    lr4_biquad_hp(&s->hp[0], sr, freq);
    lr4_biquad_hp(&s->hp[1], sr, freq);
}

static inline float lr4_split_low(lr4_split *s, float x) {
    return lr4_biquad_tick(&s->lp[1], lr4_biquad_tick(&s->lp[0], x));
}

static inline float lr4_split_high(lr4_split *s, float x) {
    return lr4_biquad_tick(&s->hp[1], lr4_biquad_tick(&s->hp[0], x));
}

/* ------------------------------------------------------------------ */
/* Main mastering chain state                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t sr;

    /* Chain parameters */
    float input_gain_db;
    float output_gain_db;
    float loudness_target_lufs;  /* e.g. -14.0 */
    float stereo_width;          /* 0=mono, 1=normal, 2=wide */
    int   bass_mono_enabled;

    /* Crossover splits */
    lr4_split split1;            /* 100 Hz  */
    lr4_split split2;            /* 1000 Hz */
    lr4_split split3;            /* 8000 Hz */

    /* Per-band compressors */
    void *comp[NBANDS];

    /* Stereo image processor */
    void *stereo_image;

    /* True-peak limiter */
    void *limiter;

    /* Loudness meter */
    wb_lufs lufs;

    
    wb_sample la_buf_l[MAX_LOOKAHEAD];
    wb_sample la_buf_r[MAX_LOOKAHEAD];
    int      la_pos;
    int      la_size;

    /* Loudness normalizer gain */
    float norm_gain;
    float peak_hold;

    /* Band output buffers (allocated at create) */
    wb_sample *band_l[NBANDS];
    wb_sample *band_r[NBANDS];
    uint32_t  buf_cap;
} wb_mastering_pro;

/* ------------------------------------------------------------------ */
/* API — public functions                                              */
/* ------------------------------------------------------------------ */

void *wb_mastering_create(uint32_t sr) {
    wb_mastering_pro *m = (wb_mastering_pro *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->sr = sr;
    m->input_gain_db = 0.0f;
    m->output_gain_db = 0.0f;
    m->loudness_target_lufs = -14.0f;
    m->stereo_width = 1.0f;
    m->bass_mono_enabled = 0;
    m->norm_gain = 1.0f;
    m->peak_hold = 0.0f;
    m->la_pos = 0;
    m->la_size = 128; /* ~2.9 ms lookahead at 44.1k */
    m->buf_cap = WB_MAX_BLOCK;

    /* Initialize crossovers */
    lr4_split_init(&m->split1, (float)sr, 100.0f);
    lr4_split_init(&m->split2, (float)sr, 1000.0f);
    lr4_split_init(&m->split3, (float)sr, 8000.0f);

    /* Per-band compressors with sensible mastering defaults */
    for (int b = 0; b < NBANDS; b++) {
        m->comp[b] = wb_comp_create(sr);
        if (m->comp[b]) {
            /* Band 0 (sub): gentle, slow */
            /* Band 1 (low-mid): moderate */
            /* Band 2 (high-mid): moderate */
            /* Band 3 (air): gentle, fast */
            switch (b) {
            case 0:
                wb_comp_set(m->comp[b], 0, -18.0f);  /* threshold */
                wb_comp_set(m->comp[b], 1, 2.0f);    /* ratio */
                wb_comp_set(m->comp[b], 2, 6.0f);    /* makeup */
                break;
            case 1:
                wb_comp_set(m->comp[b], 0, -15.0f);
                wb_comp_set(m->comp[b], 1, 3.0f);
                wb_comp_set(m->comp[b], 2, 4.0f);
                break;
            case 2:
                wb_comp_set(m->comp[b], 0, -12.0f);
                wb_comp_set(m->comp[b], 1, 2.5f);
                wb_comp_set(m->comp[b], 2, 3.0f);
                break;
            case 3:
                wb_comp_set(m->comp[b], 0, -20.0f);
                wb_comp_set(m->comp[b], 1, 2.0f);
                wb_comp_set(m->comp[b], 2, 2.0f);
                break;
            }
        }
    }

    /* Stereo image */
    m->stereo_image = wb_stereo_image_create(sr);
    if (m->stereo_image)
        wb_stereo_image_set(m->stereo_image, 0, m->stereo_width);

    /* True-peak limiter at -1 dBTP */
    m->limiter = wb_true_peak_create(sr);
    if (m->limiter)
        wb_true_peak_set(m->limiter, 0, -1.0f);

    /* Loudness meter */
    wb_lufs_create(&m->lufs, (double)sr);

    /* Allocate band buffers */
    for (int b = 0; b < NBANDS; b++) {
        m->band_l[b] = (wb_sample *)calloc(m->buf_cap, sizeof(wb_sample));
        m->band_r[b] = (wb_sample *)calloc(m->buf_cap, sizeof(wb_sample));
    }

    return m;
}

void wb_mastering_destroy(void *inst) {
    wb_mastering_pro *m = (wb_mastering_pro *)inst;
    if (!m) return;
    for (int b = 0; b < NBANDS; b++) {
        if (m->comp[b]) wb_comp_destroy(m->comp[b]);
        free(m->band_l[b]);
        free(m->band_r[b]);
    }
    if (m->stereo_image) wb_stereo_image_destroy(m->stereo_image);
    if (m->limiter) wb_true_peak_destroy(m->limiter);
    free(m);
}

void wb_mastering_set_input_gain(void *inst, float db) {
    ((wb_mastering_pro *)inst)->input_gain_db = db;
}

void wb_mastering_set_output_gain(void *inst, float db) {
    ((wb_mastering_pro *)inst)->output_gain_db = db;
}

void wb_mastering_set_loudness_target(void *inst, float lufs) {
    ((wb_mastering_pro *)inst)->loudness_target_lufs = lufs;
}

void wb_mastering_set_stereo_width(void *inst, float width) {
    wb_mastering_pro *m = (wb_mastering_pro *)inst;
    m->stereo_width = width < 0.0f ? 0.0f : (width > 2.0f ? 2.0f : width);
    if (m->stereo_image)
        wb_stereo_image_set(m->stereo_image, 0, m->stereo_width);
}

void wb_mastering_set_bass_mono(void *inst, int enable) {
    ((wb_mastering_pro *)inst)->bass_mono_enabled = enable ? 1 : 0;
}

/* Get current integrated LUFS estimate. */
float wb_mastering_get_loudness(const void *inst) {
    const wb_mastering_pro *m = (const wb_mastering_pro *)inst;
    if (!m) return 0.0f;
    double lufs = wb_lufs_integrated_lufs(&m->lufs);
    return (float)lufs;
}

/* Get current peak (linear). */
float wb_mastering_get_peak(const void *inst) {
    const wb_mastering_pro *m = (const wb_mastering_pro *)inst;
    if (!m) return 0.0f;
    return (float)wb_lufs_peak(&m->lufs);
}

/* ------------------------------------------------------------------ */
/* Process a block of interleaved-free L/R data.                        */
/* ------------------------------------------------------------------ */

void wb_mastering_process(void *inst, wb_sample *out_l, wb_sample *out_r, uint32_t frames) {
    wb_mastering_pro *m = (wb_mastering_pro *)inst;
    if (!m || !out_l || !out_r || frames == 0) return;
    if (frames > m->buf_cap) frames = m->buf_cap;

    float input_gain = powf(10.0f, m->input_gain_db / 20.0f);
    float output_gain = powf(10.0f, m->output_gain_db / 20.0f);

    /* ---- Stage 1: Input gain ---- */
    for (uint32_t i = 0; i < frames; i++) {
        out_l[i] *= input_gain;
        out_r[i] *= input_gain;
    }

    /* ---- Stage 2: 4-band crossover ---- */
    /* Split into 4 bands using the 3 LR4 crossovers.
     * Band 0: lowpass @ 100 Hz
     * Band 1: highpass @ 100 → lowpass @ 1000
     * Band 2: highpass @ 1000 → lowpass @ 8000
     * Band 3: highpass @ 8000
     *
     * We process L and R independently through the same crossover. */
    for (uint32_t i = 0; i < frames; i++) {
        float in_l = out_l[i];
        float in_r = out_r[i];

        /* First split at 100 Hz */
        float lo_l = lr4_split_low(&m->split1, in_l);
        float lo_r = lr4_split_low(&m->split1, in_r);
        float hi_l = lr4_split_high(&m->split1, in_l);
        float hi_r = lr4_split_high(&m->split1, in_r);

        /* Band 0 = lo (sub) */
        m->band_l[0][i] = lo_l;
        m->band_r[0][i] = lo_r;

        /* Second split at 1000 Hz (on the hi from first split) */
        float mid_l = lr4_split_low(&m->split2, hi_l);
        float mid_r = lr4_split_low(&m->split2, hi_r);
        float upper_l = lr4_split_high(&m->split2, hi_l);
        float upper_r = lr4_split_high(&m->split2, hi_r);

        /* Band 1 = mid (low-mid) */
        m->band_l[1][i] = mid_l;
        m->band_r[1][i] = mid_r;

        /* Third split at 8000 Hz (on the upper from second split) */
        float treble_l = lr4_split_low(&m->split3, upper_l);
        float treble_r = lr4_split_low(&m->split3, upper_r);
        float air_l = lr4_split_high(&m->split3, upper_l);
        float air_r = lr4_split_high(&m->split3, upper_r);

        /* Band 2 = treble (high-mid) */
        m->band_l[2][i] = treble_l;
        m->band_r[2][i] = treble_r;

        /* Band 3 = air */
        m->band_l[3][i] = air_l;
        m->band_r[3][i] = air_r;
    }

    /* ---- Stage 3: Per-band compression ---- */
    for (int b = 0; b < NBANDS; b++) {
        if (m->comp[b]) {
            wb_comp_process(m->comp[b], m->band_l[b], m->band_r[b], frames);
        }
    }

    /* ---- Stage 4: Bass mono (fold band 0 to mono BEFORE recombine) ---- */
    if (m->bass_mono_enabled) {
        for (uint32_t i = 0; i < frames; i++) {
            float mono = (m->band_l[0][i] + m->band_r[0][i]) * 0.5f;
            m->band_l[0][i] = mono;
            m->band_r[0][i] = mono;
        }
    }

    /* ---- Stage 5: Recombine bands ---- */
    for (uint32_t i = 0; i < frames; i++) {
        out_l[i] = m->band_l[0][i] + m->band_l[1][i] + m->band_l[2][i] + m->band_l[3][i];
        out_r[i] = m->band_r[0][i] + m->band_r[1][i] + m->band_r[2][i] + m->band_r[3][i];
    }

    /* ---- Stage 6: Stereo width (M/S processing) ---- */
    if (m->stereo_image) {
        wb_stereo_image_process(m->stereo_image, out_l, out_r, frames);
    }

    /* ---- Stage 7: Limiter (brickwall at -1 dBTP) ---- */
    if (m->limiter) {
        wb_true_peak_process(m->limiter, out_l, out_r, frames);
    }

    /* ---- Stage 8: Output gain ---- */
    for (uint32_t i = 0; i < frames; i++) {
        out_l[i] *= output_gain;
        out_r[i] *= output_gain;
    }

    /* ---- Stage 9: Loudness measurement + normalization ---- */
    /* Feed interleaved stereo to LUFS meter */
    for (uint32_t i = 0; i < frames; i++) {
        float mono = (out_l[i] + out_r[i]) * 0.5f;
        wb_lufs_process(&m->lufs, &mono, 1);
    }

    /* Compute loudness normalizer gain (slowly adapting) */
    double current_lufs = wb_lufs_integrated_lufs(&m->lufs);
    if (current_lufs < 0.0 && current_lufs > -70.0) {
        float target_gain = powf(10.0f, (float)(m->loudness_target_lufs - current_lufs) / 20.0f);
        /* Smooth adaptation: limit rate of change */
        float max_step = 1.001f; /* max 0.1% change per block */
        if (target_gain > m->norm_gain * max_step) target_gain = m->norm_gain * max_step;
        if (target_gain < m->norm_gain / max_step) target_gain = m->norm_gain / max_step;
        m->norm_gain = target_gain;
    }

    /* Apply normalization gain */
    if (m->norm_gain != 1.0f) {
        for (uint32_t i = 0; i < frames; i++) {
            out_l[i] *= m->norm_gain;
            out_r[i] *= m->norm_gain;
        }
    }

    /* Track peak */
    for (uint32_t i = 0; i < frames; i++) {
        float pk = fabsf(out_l[i]) > fabsf(out_r[i]) ? fabsf(out_l[i]) : fabsf(out_r[i]);
        if (pk > m->peak_hold) m->peak_hold = pk;
    }
}