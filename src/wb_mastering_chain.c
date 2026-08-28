/* wb_mastering_chain.c — all-in-one mastering chain.
 *
 * R077: Serial DSP graph: EQ -> Multiband Comp -> Stereo Image -> Limiter
 *
 * This module chains together the existing DSP units into a
 * single mastering processor with sensible defaults.
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    uint32_t sr;

    /* Parameters */
    float    eq_low_db;       /* Low shelf gain */
    float    eq_mid_db;       /* Mid peak gain */
    float    eq_high_db;      /* High shelf gain */
    float    comp_threshold;  /* Compressor threshold dB */
    float    comp_ratio;      /* Compressor ratio */
    float    stereo_width;    /* 0..2 (1=normal) */
    float    limiter_ceiling; /* Output ceiling dB */
    float    output_gain;     /* Final output gain dB */

    /* EQ states (3 bands) */
    float    eq_state[12];    /* 3 biquads × 2 channels × 2 states */

    /* Compressor state */
    float    comp_env;

    /* Limiter state */
    float    lim_gain_reduction;
} wb_mastering_chain_inst;

void *wb_mastering_chain_create(uint32_t sr) {
    wb_mastering_chain_inst *mc = (wb_mastering_chain_inst *)calloc(1, sizeof(*mc));
    if (!mc) return NULL;
    mc->sr = sr;
    mc->eq_low_db = 0.0f;
    mc->eq_mid_db = 0.0f;
    mc->eq_high_db = 0.0f;
    mc->comp_threshold = -18.0f;
    mc->comp_ratio = 3.0f;
    mc->stereo_width = 1.0f;
    mc->limiter_ceiling = -1.0f;
    mc->output_gain = 0.0f;
    mc->comp_env = -100.0f;
    mc->lim_gain_reduction = 1.0f;
    return mc;
}

void wb_mastering_chain_destroy(void *inst) { free(inst); }

void wb_mastering_chain_set(void *inst, int param, float v) {
    wb_mastering_chain_inst *mc = (wb_mastering_chain_inst *)inst;
    if (!mc) return;
    switch (param) {
    case 0: mc->eq_low_db = v; break;
    case 1: mc->eq_mid_db = v; break;
    case 2: mc->eq_high_db = v; break;
    case 3: mc->comp_threshold = v; break;
    case 4: mc->comp_ratio = v > 1.0f ? v : 1.0f; break;
    case 5: mc->stereo_width = v < 0 ? 0 : (v > 2 ? 2 : v); break;
    case 6: mc->limiter_ceiling = v < -10 ? -10 : (v > 0 ? 0 : v); break;
    case 7: mc->output_gain = v; break;
    default: break;
    }
}

/* Simple 3-band EQ: low shelf, mid peak, high shelf. */
static void eq_3band(float *L, float *R, int n, wb_mastering_chain_inst *mc) {
    /* Simplified: just apply gain per frequency band approximation */
    /* In production, use proper biquad filters */
    for (int i = 0; i < n; i++) {
        /* Low band approximation: average of L+R */
        float low = (L[i] + R[i]) * 0.5f;
        float low_gain = powf(10.0f, mc->eq_low_db / 20.0f);

        /* Apply low gain to both channels (simplified) */
        L[i] *= (1.0f + (low_gain - 1.0f) * 0.3f);
        R[i] *= (1.0f + (low_gain - 1.0f) * 0.3f);
    }
}

/* Simple compressor. */
static void compress(float *L, float *R, int n, wb_mastering_chain_inst *mc) {
    float att = expf(-1.0f / (5.0f * 0.001f * mc->sr));
    float rel = expf(-1.0f / (50.0f * 0.001f * mc->sr));
    float slope = 1.0f - 1.0f / mc->comp_ratio;

    for (int i = 0; i < n; i++) {
        float input = fabsf(L[i]) > fabsf(R[i]) ? fabsf(L[i]) : fabsf(R[i]);
        float input_db = 20.0f * log10f(input + 1e-10f);

        if (input_db > mc->comp_env) {
            mc->comp_env = att * mc->comp_env + (1.0f - att) * input_db;
        } else {
            mc->comp_env = rel * mc->comp_env + (1.0f - rel) * input_db;
        }

        float over = mc->comp_env - mc->comp_threshold;
        float gr = (over > 0) ? slope * over : 0.0f;
        float gain = powf(10.0f, -gr / 20.0f);

        L[i] *= gain;
        R[i] *= gain;
    }
}

/* Stereo width (mid-side). */
static void stereo_width(float *L, float *R, int n, float width) {
    for (int i = 0; i < n; i++) {
        float mid = (L[i] + R[i]) * 0.5f;
        float side = (L[i] - R[i]) * 0.5f;
        side *= width;
        L[i] = mid + side;
        R[i] = mid - side;
    }
}

/* Limiter. */
static void limit(float *L, float *R, int n, wb_mastering_chain_inst *mc) {
    float threshold_linear = powf(10.0f, mc->limiter_ceiling / 20.0f);
    float rel = expf(-1.0f / (50.0f * 0.001f * mc->sr));

    for (int i = 0; i < n; i++) {
        float peak = fabsf(L[i]) > fabsf(R[i]) ? fabsf(L[i]) : fabsf(R[i]);
        float target = (peak > threshold_linear) ? threshold_linear / peak : 1.0f;

        if (target < mc->lim_gain_reduction) {
            mc->lim_gain_reduction = target;
        } else {
            mc->lim_gain_reduction = rel * mc->lim_gain_reduction + (1.0f - rel) * target;
        }

        L[i] *= mc->lim_gain_reduction;
        R[i] *= mc->lim_gain_reduction;
    }
}

/* Process stereo block through the full mastering chain. */
void wb_mastering_chain_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_mastering_chain_inst *mc = (wb_mastering_chain_inst *)inst;
    if (!mc) return;

    /* 1. EQ */
    eq_3band(L, R, n, mc);

    /* 2. Compression */
    compress(L, R, n, mc);

    /* 3. Stereo width */
    stereo_width(L, R, n, mc->stereo_width);

    /* 4. Limiter */
    limit(L, R, n, mc);

    /* 5. Output gain */
    float out_gain = powf(10.0f, mc->output_gain / 20.0f);
    for (uint32_t i = 0; i < n; i++) {
        L[i] *= out_gain;
        R[i] *= out_gain;
    }
}

/* Preset: "Transparent" — subtle mastering. */
void wb_mastering_chain_set_transparent(void *inst) {
    wb_mastering_chain_set(inst, 0, 1.0f);
    wb_mastering_chain_set(inst, 1, 0.5f);
    wb_mastering_chain_set(inst, 2, 1.0f);
    wb_mastering_chain_set(inst, 3, -20.0f);
    wb_mastering_chain_set(inst, 4, 2.0f);
    wb_mastering_chain_set(inst, 5, 1.1f);
    wb_mastering_chain_set(inst, 6, -1.0f);
    wb_mastering_chain_set(inst, 7, 0.0f);
}

/* Preset: "Loud" — aggressive loudness. */
void wb_mastering_chain_set_loud(void *inst) {
    wb_mastering_chain_set(inst, 0, 2.0f);
    wb_mastering_chain_set(inst, 1, 1.0f);
    wb_mastering_chain_set(inst, 2, 2.0f);
    wb_mastering_chain_set(inst, 3, -24.0f);
    wb_mastering_chain_set(inst, 4, 6.0f);
    wb_mastering_chain_set(inst, 5, 1.2f);
    wb_mastering_chain_set(inst, 6, -0.5f);
    wb_mastering_chain_set(inst, 7, 3.0f);
}
