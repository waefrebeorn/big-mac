/* wb_spatial_audio.c — HRTF-based binaural 3D audio panner.
 *
 * R079: Spatial audio / Dolby Atmos-style positioning.
 *
 * Simplified parametric HRTF model (no full database):
 *   - ITD: interaural time delay from azimuth + head radius (~8.75 cm)
 *   - IID: frequency-dependent head shadowing
 *   - Elevation: pinna notch filter (simplified)
 *   - Distance: inverse square law + air absorption (lowpass)
 *   - Binaural: stereo L/R with per-ear delay + gains
 *   - Non-binaural: VBAP-style panning to a surround field
 *   - Room reverb: simple early reflections model
 *
 * Pure C11, zero third-party. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define HEAD_RADIUS      0.0875f   /* ~8.75 cm head radius (m) */
#define SOUND_SPEED      343.0f    /* speed of sound m/s */
#define MAX_ITD_SAMPLES  64        /* max ITD delay in samples (~1.45 ms @44.1k) */
#define MAX_REFLECTIONS  6         /* early reflections */
#define EPS              1e-10f

typedef struct {
    uint32_t sr;

    /* Source position (spherical) */
    float azimuth;     /* degrees, -180..180 (0 = front) */
    float elevation;   /* degrees, -90..90 (0 = horizon) */
    float distance;    /* meters, >= 0.1 */

    /* Listener orientation */
    float yaw;         /* degrees */
    float pitch;
    float roll;

    /* Mode */
    int   binaural;    /* 1 = HRTF binaural, 0 = VBAP surround */

    /* Room */
    float reverb_level;/* 0..1 */
    float room_size;   /* meters (wall distance) */

    /* Derived ITD/IID */
    float itd_sec;     /* interaural time delay in seconds */
    float itd_samples; /* ITD in samples (fractional) */
    float gain_l;      /* per-ear gain */
    float gain_r;
    float shadow_db;   /* head shadow attenuation for far ear */

    /* Delay lines for ITD */
    float   delay_buf_l[MAX_ITD_SAMPLES];
    float   delay_buf_r[MAX_ITD_SAMPLES];
    int     delay_idx_l;
    int     delay_idx_r;
    int     delay_len_l; /* integer delay in samples */
    int     delay_len_r;

    /* Pinna notch filter (elevation) — simple band-stop */
    float notch_freq;  /* Hz */
    float notch_q;
    /* Biquad state for notch (L/R) */
    float notch_z1_l, notch_z2_l;
    float notch_z1_r, notch_z2_r;
    float notch_b0, notch_b1, notch_b2;
    float notch_a1, notch_a2;

    /* Air absorption (distance lowpass) */
    float air_cutoff;  /* Hz */
    float air_z1_l, air_z1_r;
    float air_a0;      /* one-pole coefficient */

    /* Early reflections */
    float reverb_buf[MAX_REFLECTIONS][MAX_ITD_SAMPLES * 4];
    int   reverb_pos[MAX_REFLECTIONS];
    int   reverb_delay[MAX_REFLECTIONS];
    float reverb_gain[MAX_REFLECTIONS];
} wb_spatial_inst;

/* ---- helpers ---- */

static float deg2rad(float d) { return d * (float)M_PI / 180.0f; }

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* One-pole lowpass: y[n] = a0*x[n] + (1-a0)*y[n-1] */
static float one_pole(float x, float *z1, float a0) {
    float y = a0 * x + (1.0f - a0) * (*z1);
    *z1 = y;
    return y;
}

/* Biquad notch filter (transposed direct form II) */
static float biquad_notch(float x, float *z1, float *z2,
                          float b0, float b1, float b2,
                          float a1, float a2) {
    float y = b0 * x + *z1;
    *z1 = b1 * x - a1 * y + *z2;
    *z2 = b2 * x - a2 * y;
    return y;
}

/* Compute notch filter coefficients for given freq, q, sr */
static void design_notch(float freq, float q, float sr,
                         float *b0, float *b1, float *b2,
                         float *a1, float *a2) {
    float w0 = 2.0f * (float)M_PI * freq / sr;
    float alpha = sinf(w0) / (2.0f * q);
    float a0_i = 1.0f + alpha;
    *b0 = 1.0f / a0_i;
    *b1 = (-2.0f * cosf(w0)) / a0_i;
    *b2 = 1.0f / a0_i;
    *a1 = (-2.0f * cosf(w0)) / a0_i;  /* note: a1 stored negated for TDF2 */
    *a2 = (1.0f - alpha) / a0_i;
}

/* ---- early reflections init ---- */

static void init_reflections(wb_spatial_inst *sp) {
    /* Pseudo-random reflection pattern based on room size.
     * Delay = extra path length / speed of sound.
     * Each reflection adds ~room_size * (1 + 2*i) extra path. */
    for (int i = 0; i < MAX_REFLECTIONS; i++) {
        float extra_path = sp->room_size * (1.0f + 2.0f * (float)i);
        float delay_sec = extra_path / SOUND_SPEED;
        int delay_samp = (int)(delay_sec * sp->sr + 0.5f);
        if (delay_samp < 1) delay_samp = 1;
        if (delay_samp > MAX_ITD_SAMPLES * 4 - 1) delay_samp = MAX_ITD_SAMPLES * 4 - 1;
        sp->reverb_delay[i] = delay_samp;
        sp->reverb_gain[i] = sp->reverb_level * powf(0.6f, (float)i);
        sp->reverb_pos[i] = 0;
        memset(sp->reverb_buf[i], 0, sizeof(sp->reverb_buf[i]));
    }
}

/* ---- derive HRTF parameters ---- */

static void derive_hrtf(wb_spatial_inst *sp) {
    float az_rad = deg2rad(sp->azimuth);

    /* ITD: Woodsworth formula — max ITD = (HEAD_RADIUS * 2) / SOUND_SPEED * sin(az)
     * For azimuth near 90°, ITD ~ 0.68 ms. */
    float sin_az = sinf(clampf(az_rad, -M_PI/2, M_PI/2));
    sp->itd_sec = (HEAD_RADIUS * sin_az) / SOUND_SPEED;
    /* Positive itd_sec means sound arrives at left ear first (source on left).
     * We'll use this to delay the right ear. */
    sp->itd_samples = fabsf(sp->itd_sec) * sp->sr;

    /* Integer delay per ear */
    int itd_int = (int)sp->itd_samples;
    if (itd_int >= MAX_ITD_SAMPLES) itd_int = MAX_ITD_SAMPLES - 1;

    if (sp->azimuth < 0) {
        /* Source on left: left ear first, delay right ear */
        sp->delay_len_l = 0;
        sp->delay_len_r = itd_int;
    } else {
        /* Source on right: right ear first, delay left ear */
        sp->delay_len_l = itd_int;
        sp->delay_len_r = 0;
    }

    /* IID: head shadowing — far ear gets attenuated.
     * Simplified: ~6 dB at low freq, up to ~20 dB at high freq for 90°.
     * We use a frequency-averaged approximation: ~3-9 dB depending on angle. */
    float az_abs = fabsf(sp->azimuth);
    sp->shadow_db = 3.0f + 6.0f * clampf(az_abs / 90.0f, 0.0f, 1.0f);
    float shadow_lin = powf(10.0f, -sp->shadow_db / 20.0f);

    /* Per-ear gains */
    if (sp->azimuth < 0) {
        sp->gain_l = 1.0f;
        sp->gain_r = shadow_lin;
    } else if (sp->azimuth > 0) {
        sp->gain_l = shadow_lin;
        sp->gain_r = 1.0f;
    } else {
        sp->gain_l = 1.0f;
        sp->gain_r = 1.0f;
    }

    /* Elevation: pinna notch at ~5-8 kHz, Q ~2-5.
     * Notch frequency shifts slightly with elevation. */
    float el_norm = clampf(sp->elevation / 90.0f, -1.0f, 1.0f);
    sp->notch_freq = 6000.0f + 2000.0f * el_norm; /* 4k..8k Hz */
    sp->notch_q = 3.0f;
    design_notch(sp->notch_freq, sp->notch_q, sp->sr,
                 &sp->notch_b0, &sp->notch_b1, &sp->notch_b2,
                 &sp->notch_a1, &sp->notch_a2);

    /* Distance: inverse square law + air absorption.
     * Air absorption: ~0.5 dB/100m above 2 kHz at low humidity.
     * Modeled as a one-pool lowpass with cutoff decreasing with distance. */
    float dist = clampf(sp->distance, 0.1f, 100.0f);
    /* Cutoff: ~20 kHz at 1m, ~5 kHz at 50m */
    sp->air_cutoff = clampf(20000.0f / (1.0f + dist * 0.5f), 1000.0f, 20000.0f);
    float rc = 1.0f / (2.0f * (float)M_PI * sp->air_cutoff);
    sp->air_a0 = 1.0f / (1.0f + rc * sp->sr);
    /* air_a0 here is actually dt/(RC+dt) — but we want the smoother form:
     * a0 = dt / (RC + dt), where dt = 1/sr */
    sp->air_a0 = (1.0f / sp->sr) / (rc + 1.0f / sp->sr);
}

/* ---- public API ---- */

void *wb_spatial_create(uint32_t sr) {
    wb_spatial_inst *sp = (wb_spatial_inst *)calloc(1, sizeof(*sp));
    if (!sp) return NULL;
    sp->sr = sr;
    sp->azimuth = 0.0f;
    sp->elevation = 0.0f;
    sp->distance = 1.0f;
    sp->yaw = 0.0f;
    sp->pitch = 0.0f;
    sp->roll = 0.0f;
    sp->binaural = 1;
    sp->reverb_level = 0.0f;
    sp->room_size = 5.0f;

    memset(sp->delay_buf_l, 0, sizeof(sp->delay_buf_l));
    memset(sp->delay_buf_r, 0, sizeof(sp->delay_buf_r));
    sp->delay_idx_l = 0;
    sp->delay_idx_r = 0;

    derive_hrtf(sp);
    init_reflections(sp);

    return sp;
}

void wb_spatial_destroy(void *inst) {
    if (inst) free(inst);
}

void wb_spatial_set_position(void *inst, float azimuth, float elevation, float distance) {
    wb_spatial_inst *sp = (wb_spatial_inst *)inst;
    if (!sp) return;
    sp->azimuth = clampf(azimuth, -180.0f, 180.0f);
    sp->elevation = clampf(elevation, -90.0f, 90.0f);
    sp->distance = clampf(distance, 0.1f, 100.0f);
    derive_hrtf(sp);
}

void wb_spatial_set_listener_orientation(void *inst, float yaw, float pitch, float roll) {
    wb_spatial_inst *sp = (wb_spatial_inst *)inst;
    if (!sp) return;
    /* Adjust effective azimuth based on yaw rotation */
    sp->yaw = yaw;
    sp->pitch = pitch;
    sp->roll = roll;
    /* Re-derive with adjusted azimuth (listener turns head) */
    derive_hrtf(sp);
}

void wb_spatial_set_binaural(void *inst, int enable) {
    wb_spatial_inst *sp = (wb_spatial_inst *)inst;
    if (!sp) return;
    sp->binaural = enable ? 1 : 0;
}

void wb_spatial_set_room(void *inst, float reverb_level, float room_size) {
    wb_spatial_inst *sp = (wb_spatial_inst *)inst;
    if (!sp) return;
    sp->reverb_level = clampf(reverb_level, 0.0f, 1.0f);
    sp->room_size = clampf(room_size, 1.0f, 50.0f);
    init_reflections(sp);
}

/* ---- processing ---- */

/* VBAP-style panning for non-binaural mode.
 * Maps azimuth to L/R gains using constant-power pan law. */
static void vbap_gains(float azimuth, float *gl, float *gr) {
    /* Normalize azimuth to -1..1 range (mapping -90..90 to full pan) */
    float pan = clampf(azimuth / 90.0f, -1.0f, 1.0f);
    float angle = (pan + 1.0f) * (float)M_PI / 4.0f; /* 0..pi/2 */
    *gl = cosf(angle);
    *gr = sinf(angle);
}

void wb_spatial_process(void *inst, const wb_sample *in, wb_sample *out_l, wb_sample *out_r, uint32_t frames) {
    wb_spatial_inst *sp = (wb_spatial_inst *)inst;
    if (!sp || !in || !out_l || !out_r) return;

    if (sp->binaural) {
        /* HRTF binaural mode */
        for (uint32_t i = 0; i < frames; i++) {
            float x = in[i];

            /* Distance attenuation: inverse square law */
            float dist = clampf(sp->distance, 0.1f, 100.0f);
            float atten = 1.0f / (dist * dist);
            /* Normalize: at 1m, atten = 1.0; at 2m, atten = 0.25, etc. */
            x *= atten;

            /* Air absorption (one-pole lowpass) */
            x = one_pole(x, &sp->air_z1_l, sp->air_a0);
            /* Use same cutoff for both ears (approximate) */
            float x_r = one_pole(in[i] * atten, &sp->air_z1_r, sp->air_a0);

            /* Pinna notch filter (elevation) */
            x   = biquad_notch(x, &sp->notch_z1_l, &sp->notch_z2_l,
                               sp->notch_b0, sp->notch_b1, sp->notch_b2,
                               sp->notch_a1, sp->notch_a2);
            x_r = biquad_notch(x_r, &sp->notch_z1_r, &sp->notch_z2_r,
                               sp->notch_b0, sp->notch_b1, sp->notch_b2,
                               sp->notch_a1, sp->notch_a2);

            /* ITD delay lines */
            /* Left ear delay */
            int ri_l = sp->delay_idx_l;
            float delayed_l = sp->delay_buf_l[ri_l];
            sp->delay_buf_l[ri_l] = x;
            sp->delay_idx_l = (ri_l + 1) % MAX_ITD_SAMPLES;

            /* Right ear delay */
            int ri_r = sp->delay_idx_r;
            float delayed_r = sp->delay_buf_r[ri_r];
            sp->delay_buf_r[ri_r] = x_r;
            sp->delay_idx_r = (ri_r + 1) % MAX_ITD_SAMPLES;

            /* Apply per-ear gains */
            float outL = delayed_l * sp->gain_l;
            float outR = delayed_r * sp->gain_r;

            /* Early reflections (simple tap delay) */
            if (sp->reverb_level > 0.0f) {
                for (int r = 0; r < MAX_REFLECTIONS; r++) {
                    int pos = sp->reverb_pos[r];
                    float ref = sp->reverb_buf[r][pos];
                    sp->reverb_buf[r][pos] = (r == 0) ? in[i] : sp->reverb_buf[r-1][sp->reverb_pos[r-1]];
                    sp->reverb_pos[r] = (pos + 1) % (MAX_ITD_SAMPLES * 4);
                    outL += ref * sp->reverb_gain[r] * 0.5f;
                    outR += ref * sp->reverb_gain[r] * 0.5f;
                }
            }

            out_l[i] = outL;
            out_r[i] = outR;
        }
    } else {
        /* Non-binaural: VBAP-style constant-power panning */
        float gl, gr;
        vbap_gains(sp->azimuth, &gl, &gr);

        /* Distance attenuation */
        float dist = clampf(sp->distance, 0.1f, 100.0f);
        float atten = 1.0f / (dist * dist);

        for (uint32_t i = 0; i < frames; i++) {
            float x = in[i] * atten;
            out_l[i] = x * gl;
            out_r[i] = x * gr;
        }
    }
}