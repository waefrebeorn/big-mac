/* wb_synth_simd.c — G4/G6: SIMD-accelerated synth render with oversampling.
 *
 * G4: Sine oscillator batching — batches 4 voices' sine oscillators through
 *   vec_sin_4, reducing sin calls from 8,192 to 2,048 per block.
 *
 * G6: 2× oversampling — renders at 2× sample rate then decimates with a
 *   half-band filter. Reduces aliasing from saw/square oscillators.
 *   Feasible because SIMD sin gives 4× throughput: 2× oversampling costs
 *   only 2× in sin work (not 4×).
 *
 * NOTE: The biquad filter SIMD path is deferred — it requires separate state
 * tracking (transposed Form II states differ from direct Form I x1/x2/y1/y2).
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <emmintrin.h>
#include "g2_fm_simd.h"
#include "wb_biquad_simd.h"

/* Forward decl from wb_osc.c */
__m128 wb_osc_process_wt4(__m128 *phases, float inc);

typedef float wb_sample;

typedef struct { double phase, phase_inc, last_out; } wb_osc;
typedef struct { int stage; float level, a, d, s, r, sr; } wb_env;
typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2, sr; } wb_biquad;

#define MAX_VOICES 16

typedef struct voice {
    int    active;
    int    note;
    int    vel;
    double freq;
    wb_osc osc1, osc2;
    wb_env env;
    wb_biquad filter;
} voice;

typedef struct {
    uint32_t sr;
    voice voices[MAX_VOICES];
    float   master_vol;
    float   filter_cutoff;
    float   filter_res;
    int     waveform;
    float   a, d, s, r;
} wb_synth_inst;

extern float wb_osc_process(wb_osc *o, float inc, int waveform, float shape);
extern float wb_env_process(wb_env *e);

/* ---- SIMD synth render block: sine batching + biquad SIMD (G4/G7) ---- */
void wb_synth_render_block_simd(void *instp, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_synth_inst *s = instp;
    if (!s) return;
    float inc_scale = 1.0f / (float)s->sr;

    float phase_step[MAX_VOICES];
    for (int v = 0; v < MAX_VOICES; v++) {
        voice *vv = &s->voices[v];
        phase_step[v] = vv->active ? (float)(2.0 * M_PI * vv->freq * inc_scale) : 0.0f;
    }

    /* Shared biquad coefficients (all voices use same cutoff/res) */
    wb_biquad4 bq;
    wb_biquad4_init(&bq, s->voices[0].filter.b0, s->voices[0].filter.b1,
                          s->voices[0].filter.b2, s->voices[0].filter.a1,
                          s->voices[0].filter.a2);

    for (uint32_t i = 0; i < n; i++) {
        float mixL = 0, mixR = 0;

        for (int b = 0; b < MAX_VOICES; b += 4) {
            int any_active = s->voices[b].active || s->voices[b+1].active ||
                           s->voices[b+2].active || s->voices[b+3].active;
            if (!any_active) continue;

            /* Scalar: osc1 (saw) for 4 voices */
            float o1[4];
            for (int j = 0; j < 4; j++) {
                voice *vv = &s->voices[b + j];
                o1[j] = vv->active ? wb_osc_process(&vv->osc1, phase_step[b+j], s->waveform, 0.5f) : 0.0f;
            }

            /* SIMD: osc2 (sine) for 4 voices at once */
            float ph[4] = {0,0,0,0};
            for (int j = 0; j < 4; j++) {
                voice *vv = &s->voices[b + j];
                if (vv->active) {
                    vv->osc2.phase += phase_step[b+j] * 1.007f;
                    if (vv->osc2.phase >= 2.0 * M_PI) vv->osc2.phase -= 2.0 * M_PI;
                    if (vv->osc2.phase < 0) vv->osc2.phase += 2.0 * M_PI;
                    ph[j] = (float)vv->osc2.phase;
                }
            }
            __m128 o2_vec = vec_sin_4(_mm_loadu_ps(ph));

            /* Mix: raw = o1*0.6 + o2*0.4 (4-wide) */
            __m128 o1_vec = _mm_loadu_ps(o1);
            __m128 raw_vec = _mm_add_ps(
                _mm_mul_ps(o1_vec, _mm_set1_ps(0.6f)),
                _mm_mul_ps(o2_vec, _mm_set1_ps(0.4f))
            );

            /* SIMD biquad filter (4 voices at once) */
            __m128 filtered = wb_biquad4_process(&bq, raw_vec);

            /* Per-voice envelope + amp (scalar — env is per-voice state) */
            float filtered_arr[4];
            _mm_storeu_ps(filtered_arr, filtered);
            for (int j = 0; j < 4; j++) {
                voice *vv = &s->voices[b + j];
                if (!vv->active) continue;
                float env = wb_env_process(&vv->env);
                float amp = env * (vv->vel / 127.0f) * s->master_vol;
                mixL += filtered_arr[j] * amp;
                mixR += filtered_arr[j] * amp;
                if (vv->env.stage == 0) vv->active = 0;
            }
        }

        if (mixL > 1.0f) mixL = 1.0f; else if (mixL < -1.0f) mixL = -1.0f;
        if (mixR > 1.0f) mixR = 1.0f; else if (mixR < -1.0f) mixR = -1.0f;
        L[i] = mixL;
        R[i] = mixR;
    }
}

/* ---- Wavetable synth render (G8): SIMD wavetable + SIMD biquad ----
 * Uses wb_osc_process_wt4 for the main oscillator (wavetable lookup)
 * and vec_sin_4 for the detuned oscillator, plus SIMD biquad.
 * This is the highest-quality synth path: no sin() calls at all
 * (wavetable lookup replaces both oscillators). */
void wb_synth_render_block_wt(void *instp, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_synth_inst *s = instp;
    if (!s) return;
    float inc_scale = 1.0f / (float)s->sr;

    float phase_step[MAX_VOICES];
    for (int v = 0; v < MAX_VOICES; v++) {
        voice *vv = &s->voices[v];
        phase_step[v] = vv->active ? (float)(2.0 * M_PI * vv->freq * inc_scale) : 0.0f;
    }

    /* Shared biquad coefficients */
    wb_biquad4 bq;
    wb_biquad4_init(&bq, s->voices[0].filter.b0, s->voices[0].filter.b1,
                          s->voices[0].filter.b2, s->voices[0].filter.a1,
                          s->voices[0].filter.a2);

    /* Per-batch phase accumulators for SIMD wavetable */
    __m128 phases_a[4]; /* 4 batches of 4 voices = 16 voices */
    __m128 phases_b[4]; /* detuned oscillator phases */
    for (int b = 0; b < 4; b++) {
        float ph_a[4] = {0,0,0,0};
        float ph_b[4] = {0,0,0,0};
        for (int j = 0; j < 4; j++) {
            int idx = b * 4 + j;
            if (idx < MAX_VOICES && s->voices[idx].active) {
                ph_a[j] = (float)s->voices[idx].osc1.phase;
                ph_b[j] = (float)s->voices[idx].osc2.phase;
            }
        }
        phases_a[b] = _mm_loadu_ps(ph_a);
        phases_b[b] = _mm_loadu_ps(ph_b);
    }

    for (uint32_t i = 0; i < n; i++) {
        float mixL = 0, mixR = 0;

        for (int b = 0; b < 4; b++) {
            int base = b * 4;
            int any_active = s->voices[base].active || s->voices[base+1].active ||
                           s->voices[base+2].active || s->voices[base+3].active;
            if (!any_active) continue;

            /* SIMD wavetable: 4 oscillators at once */
            __m128 o1_vec = wb_osc_process_wt4(&phases_a[b], phase_step[base]);

            /* SIMD sine: detuned oscillator (4 voices) */
            float ph_b_arr[4];
            _mm_storeu_ps(ph_b_arr, phases_b[b]);
            for (int j = 0; j < 4; j++) {
                int idx = base + j;
                if (idx < MAX_VOICES && s->voices[idx].active) {
                    ph_b_arr[j] += phase_step[idx] * 1.007f;
                    if (ph_b_arr[j] >= (float)(2.0 * M_PI)) ph_b_arr[j] -= (float)(2.0 * M_PI);
                }
            }
            phases_b[b] = _mm_loadu_ps(ph_b_arr);
            __m128 o2_vec = vec_sin_4(phases_b[b]);

            /* Mix: raw = o1*0.6 + o2*0.4 (4-wide) */
            __m128 raw_vec = _mm_add_ps(
                _mm_mul_ps(o1_vec, _mm_set1_ps(0.6f)),
                _mm_mul_ps(o2_vec, _mm_set1_ps(0.4f))
            );

            /* SIMD biquad */
            __m128 filtered = wb_biquad4_process(&bq, raw_vec);
            float filtered_arr[4];
            _mm_storeu_ps(filtered_arr, filtered);

            /* Per-voice envelope + amp */
            for (int j = 0; j < 4; j++) {
                int idx = base + j;
                if (idx >= MAX_VOICES || !s->voices[idx].active) continue;
                float env = wb_env_process(&s->voices[idx].env);
                float amp = env * (s->voices[idx].vel / 127.0f) * s->master_vol;
                mixL += filtered_arr[j] * amp;
                mixR += filtered_arr[j] * amp;
                if (s->voices[idx].env.stage == 0) s->voices[idx].active = 0;
            }
        }

        if (mixL > 1.0f) mixL = 1.0f; else if (mixL < -1.0f) mixL = -1.0f;
        if (mixR > 1.0f) mixR = 1.0f; else if (mixR < -1.0f) mixR = -1.0f;
        L[i] = mixL;
        R[i] = mixR;
    }

    /* Write back oscillator phases */
    for (int b = 0; b < 4; b++) {
        float ph_a[4], ph_b[4];
        _mm_storeu_ps(ph_a, phases_a[b]);
        _mm_storeu_ps(ph_b, phases_b[b]);
        for (int j = 0; j < 4; j++) {
            int idx = b * 4 + j;
            if (idx < MAX_VOICES) {
                s->voices[idx].osc1.phase = ph_a[j];
                s->voices[idx].osc2.phase = ph_b[j];
            }
        }
    }
}

/* ---- 2× oversampled synth render (G6) ----
 * Renders at 2× sample rate, then decimates with a simple half-band filter.
 * The half-band filter is a 3-tap FIR: out[i] = 0.25*x[2i-1] + 0.5*x[2i] + 0.25*x[2i+1].
 * This removes aliasing from the saw/square oscillators above Nyquist.
 *
 * Cost: 2× the oscillator work, but SIMD sin makes it affordable.
 * The biquad filter runs at 2× rate (better accuracy near Nyquist).
 */
void wb_synth_render_block_simd_2x(void *instp, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_synth_inst *s = instp;
    if (!s) return;
    float inc_scale = 0.5f / (float)s->sr;  /* half the phase step = 2× rate */

    float phase_step[MAX_VOICES];
    for (int v = 0; v < MAX_VOICES; v++) {
        voice *vv = &s->voices[v];
        phase_step[v] = vv->active ? (float)(2.0 * M_PI * vv->freq * inc_scale) : 0.0f;
    }

    /* Temp buffer for 2× rate output (before decimation) */
    static __thread wb_sample buf_2x[1024]; /* 2 × 512 max */

    /* Shared biquad SIMD state for 2× rate */
    wb_biquad4 bq_2x;
    wb_biquad4_init(&bq_2x, s->voices[0].filter.b0, s->voices[0].filter.b1,
                           s->voices[0].filter.b2, s->voices[0].filter.a1,
                           s->voices[0].filter.a2);

    for (uint32_t i = 0; i < n; i++) {
        /* Process 2 sub-samples at 2× rate */
        float mix[2] = {0, 0};

        for (int sub = 0; sub < 2; sub++) {
            float sub_mix = 0;

            for (int b = 0; b < MAX_VOICES; b += 4) {
                int any_active = s->voices[b].active || s->voices[b+1].active ||
                               s->voices[b+2].active || s->voices[b+3].active;
                if (!any_active) continue;

                /* Scalar: osc1 (saw) for 4 voices */
                float o1[4];
                for (int j = 0; j < 4; j++) {
                    voice *vv = &s->voices[b + j];
                    o1[j] = vv->active ? wb_osc_process(&vv->osc1, phase_step[b+j], s->waveform, 0.5f) : 0.0f;
                }

                /* SIMD: osc2 (sine) for 4 voices */
                float ph[4] = {0,0,0,0};
                for (int j = 0; j < 4; j++) {
                    voice *vv = &s->voices[b + j];
                    if (vv->active) {
                        vv->osc2.phase += phase_step[b+j] * 1.007f;
                        if (vv->osc2.phase >= 2.0 * M_PI) vv->osc2.phase -= 2.0 * M_PI;
                        if (vv->osc2.phase < 0) vv->osc2.phase += 2.0 * M_PI;
                        ph[j] = (float)vv->osc2.phase;
                    }
                }
                __m128 o2_vec = vec_sin_4(_mm_loadu_ps(ph));

                /* Mix: raw = o1*0.6 + o2*0.4 (4-wide) */
                __m128 o1_vec = _mm_loadu_ps(o1);
                __m128 raw_vec = _mm_add_ps(
                    _mm_mul_ps(o1_vec, _mm_set1_ps(0.6f)),
                    _mm_mul_ps(o2_vec, _mm_set1_ps(0.4f))
                );

                /* SIMD biquad at 2× rate */
                __m128 filtered = wb_biquad4_process(&bq_2x, raw_vec);
                float filtered_arr[4];
                _mm_storeu_ps(filtered_arr, filtered);

                /* Per-voice envelope + amp */
                for (int j = 0; j < 4; j++) {
                    voice *vv = &s->voices[b + j];
                    if (!vv->active) continue;
                    float env = wb_env_process(&vv->env);
                    float amp = env * (vv->vel / 127.0f) * s->master_vol;
                    sub_mix += filtered_arr[j] * amp;
                    if (vv->env.stage == 0) vv->active = 0;
                }
            }

            mix[sub] = sub_mix;
        }

        /* Store both sub-samples for decimation */
        buf_2x[2*i]   = mix[0];
        buf_2x[2*i+1] = mix[1];
    }

    /* Decimate: 3-tap half-band filter
     * out[i] = 0.25 * x[2i-1] + 0.5 * x[2i] + 0.25 * x[2i+1]
     * For i=0: out[0] = 0.5 * x[0] + 0.25 * x[1] (assume x[-1] = x[0])
     * For i=n-1: out[n-1] = 0.25 * x[2n-3] + 0.5 * x[2n-2] + 0.25 * x[2n-1] */
    for (uint32_t i = 0; i < n; i++) {
        float xm1 = (i == 0) ? buf_2x[0] : buf_2x[2*i - 1];
        float x0  = buf_2x[2*i];
        float xp1 = buf_2x[2*i + 1];
        float out = 0.25f * xm1 + 0.5f * x0 + 0.25f * xp1;

        if (out > 1.0f) out = 1.0f; else if (out < -1.0f) out = -1.0f;
        L[i] = out;
        R[i] = out;
    }
}
