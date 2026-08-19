/* wb_voice_polish.c — one-click voice-polish preset chain (R015 Tier 1).
 *
 * Chain: gate -> deesser -> comp -> tilt EQ -> limiter -> loudness norm.
 * Pure C11, all ours. Reuses wb_biquad for the EQ stage.
 */

#include "wbus/wbus_voice_polish.h"
#include "wbus/wbus_dsp.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- per-channel DSP state -------------------------------------------- */
typedef struct {
    /* gate */
    float   gate_env;       /* envelope follower (rectified) */
    int     gate_open;
    /* deesser */
    wb_biquad deess_band;    /* high band for sibilance detection */
    float   deess_env;
    /* compressor */
    float   comp_env;        /* level detector */
    float   comp_gain;       /* smoothed gain */
    /* limiter */
    float   lim_env;
    /* EQ */
    wb_biquad low_cut;       /* highpass ~80 Hz */
    wb_biquad presence;      /* peaking ~3 kHz */
} vp_chan;

struct wb_voice_polish {
    float sr;
    int   channels;
    vp_chan *ch;
    /* shared params */
    float gate_thresh;       /* -dBFS */
    float deess_thresh;      /* ratio engage threshold, dB */
    float comp_thresh;       /* -dBFS */
    float comp_ratio;
    float lim_ceiling;       /* -dBFS */
    float target_lufs;
};

/* ---- helpers ----------------------------------------------------------- */

static inline float db_to_lin(float db) { return (float)powf(10.0f, db / 20.0f); }
static inline float lin_to_db(float x) { return 20.0f * (float)log10f(x + 1e-9f); }

/* one-pole envelope follower: attack/release time constants (s) */
static inline void env_follow(float *state, float x, float atk_s, float rel_s,
                              float sr) {
    float a = (float)expf(-1.0f / (atk_s * sr));
    float r = (float)expf(-1.0f / (rel_s * sr));
    float coeff = (x > *state) ? a : r;
    *state = x + coeff * (*state - x);
}

/* ---- create / free ----------------------------------------------------- */

wb_voice_polish *wb_voice_polish_create(float sample_rate, int channels) {
    if (channels <= 0 || channels > 8) return NULL;
    wb_voice_polish *vp = calloc(1, sizeof(*vp));
    if (!vp) return NULL;
    vp->ch = calloc((size_t)channels, sizeof(vp_chan));
    if (!vp->ch) { free(vp); return NULL; }
    vp->sr = sample_rate;
    vp->channels = channels;
    vp->gate_thresh = -45.0f;
    vp->deess_thresh = -24.0f;
    vp->comp_thresh = -18.0f;
    vp->comp_ratio = 3.0f;
    vp->lim_ceiling = -1.0f;
    vp->target_lufs = -16.0f;

    for (int c = 0; c < channels; c++) {
        wb_biquad_init(&vp->ch[c].low_cut, sample_rate);
        wb_biquad_set(&vp->ch[c].low_cut, 1, 80.0f, 0.7f, 0.0f);   /* highpass */
        wb_biquad_init(&vp->ch[c].presence, sample_rate);
        wb_biquad_set(&vp->ch[c].presence, 3, 3000.0f, 1.0f, 3.0f); /* peaking +3dB */
        wb_biquad_init(&vp->ch[c].deess_band, sample_rate);
        /* bandpass around 6 kHz for sibilance detection */
        wb_biquad_set(&vp->ch[c].deess_band, 2, 6000.0f, 1.2f, 0.0f);
    }
    return vp;
}

void wb_voice_polish_free(wb_voice_polish *vp) {
    if (!vp) return;
    free(vp->ch);
    free(vp);
}

void wb_voice_polish_reset(wb_voice_polish *vp) {
    if (!vp) return;
    for (int c = 0; c < vp->channels; c++) {
        memset(&vp->ch[c], 0, sizeof(vp_chan));
        wb_biquad_init(&vp->ch[c].low_cut, vp->sr);
        wb_biquad_set(&vp->ch[c].low_cut, 1, 80.0f, 0.7f, 0.0f);
        wb_biquad_init(&vp->ch[c].presence, vp->sr);
        wb_biquad_set(&vp->ch[c].presence, 3, 3000.0f, 1.0f, 3.0f);
        wb_biquad_init(&vp->ch[c].deess_band, vp->sr);
        wb_biquad_set(&vp->ch[c].deess_band, 2, 6000.0f, 1.2f, 0.0f);
    }
}

/* ---- per-sample processing for one channel ---------------------------- */

static inline float process_chan(vp_chan *s, wb_voice_polish *vp, float x) {
    /* 1. noise gate: open only when level exceeds threshold */
    float lvl = (float)fabsf(x);
    env_follow(&s->gate_env, lvl, 0.005f, 0.05f, vp->sr);
    float gt = db_to_lin(vp->gate_thresh);
    int want_open = (s->gate_env > gt) ? 1 : 0;
    /* hysteresis: keep open briefly */
    if (want_open) s->gate_open = 1;
    else if (s->gate_env < gt * 0.5f) s->gate_open = 0;
    float gate_gain = s->gate_open ? 1.0f : 0.0f;
    x *= gate_gain;

    /* 2. de-esser: detect sibilance energy in 6 kHz band, duck that band */
    float band = wb_biquad_process(&s->deess_band, x);
    float b_energy = (float)fabsf(band);
    env_follow(&s->deess_env, b_energy, 0.002f, 0.05f, vp->sr);
    float ds_thr = db_to_lin(vp->deess_thresh);
    float duck = 1.0f;
    if (s->deess_env > ds_thr) {
        float over = s->deess_env / ds_thr;
        duck = (float)powf(over, -1.5f);          /* up to -9 dB reduction */
        if (duck < 0.35f) duck = 0.35f;
    }
    /* apply duck to the high band, keep low band untouched */
    x = x - band + band * duck;

    /* 3. compressor: level-triggered gain reduction */
    float lvl2 = (float)fabsf(x);
    env_follow(&s->comp_env, lvl2, 0.003f, 0.12f, vp->sr);
    float ct = db_to_lin(vp->comp_thresh);
    float new_gain = 1.0f;
    if (s->comp_env > ct) {
        float over_db = lin_to_db(s->comp_env) - lin_to_db(ct);
        float red_db = over_db * (1.0f - 1.0f / vp->comp_ratio);
        new_gain = db_to_lin(-red_db);
    }
    /* smooth gain to avoid zipper noise */
    s->comp_gain += 0.002f * (new_gain - s->comp_gain);
    x *= s->comp_gain;

    /* 4. EQ: low-cut + presence */
    x = wb_biquad_process(&s->low_cut, x);
    x = wb_biquad_process(&s->presence, x);

    /* 5. limiter: hard ceiling with fast detection */
    float lvl3 = (float)fabsf(x);
    env_follow(&s->lim_env, lvl3, 0.0005f, 0.005f, vp->sr);
    float ceil = db_to_lin(vp->lim_ceiling);
    if (s->lim_env > ceil) {
        float g = ceil / s->lim_env;
        x *= g;
    }
    return x;
}

/* ---- block process ----------------------------------------------------- */

int wb_voice_polish_process(wb_voice_polish *vp, float *buf, uint32_t frames) {
    if (!vp || !buf) return -1;
    for (uint32_t i = 0; i < frames; i++) {
        for (int c = 0; c < vp->channels; c++) {
            float *s = buf + (size_t)i * vp->channels + c;
            *s = process_chan(&vp->ch[c], vp, *s);
        }
    }
    return 0;
}

/* ---- loudness (BS.1770-4 K-weighting, simplified) ---------------------- */

float wb_loudness_measure(const float *buf, uint32_t frames, int channels,
                           float sample_rate) {
    if (!buf || channels <= 0) return -1e9f;
    /* K-weighting: RLB high-shelf (~ +4 dB @ 2 kHz) + highpass ~38 Hz.
     * Approximated with two biquads per channel. */
    wb_biquad hp[8], hs[8];
    for (int c = 0; c < channels && c < 8; c++) {
        wb_biquad_init(&hp[c], sample_rate);
        wb_biquad_set(&hp[c], 1, 38.0f, 0.5f, 0.0f);     /* highpass */
        wb_biquad_init(&hs[c], sample_rate);
        wb_biquad_set(&hs[c], 3, 2000.0f, 1.0f, 4.0f);   /* presence +4dB */
    }
    double sum = 0.0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < frames; i++) {
        for (int c = 0; c < channels; c++) {
            float x = buf[(size_t)i * channels + c];
            x = wb_biquad_process(&hp[c], x);
            x = wb_biquad_process(&hs[c], x);
            sum += (double)x * x;
            n++;
        }
    }
    if (n == 0) return -1e9f;
    double mean_sq = sum / n;
    /* BS.1770: 10*log10(mean_sq) + calibration offset (-0.691 dB) */
    return (float)(10.0 * log10(mean_sq + 1e-12) - 0.691);
}

/* ---- convenience: gate->...->limiter, then scale to target LUFS -------- */

int wb_voice_polish_apply(float *buf, uint32_t frames, int channels,
                           float sample_rate, float target_lufs) {
    if (!buf || channels <= 0) return -1;
    wb_voice_polish *vp = wb_voice_polish_create(sample_rate, channels);
    if (!vp) return -1;
    vp->target_lufs = target_lufs;
    wb_voice_polish_process(vp, buf, frames);
    /* measure post-chain loudness, scale to target */
    float meas = wb_loudness_measure(buf, frames, channels, sample_rate);
    float delta_db = target_lufs - meas;
    float g = db_to_lin(delta_db);
    /* clamp safety gain */
    if (g > 4.0f) g = 4.0f;
    for (uint32_t i = 0; i < frames * (uint32_t)channels; i++) {
        buf[i] *= g;
        if (buf[i] > 1.0f) buf[i] = 1.0f;
        if (buf[i] < -1.0f) buf[i] = -1.0f;
    }
    wb_voice_polish_free(vp);
    return 0;
}
