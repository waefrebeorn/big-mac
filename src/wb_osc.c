/* wb_osc.c — oscillators: sine/saw/square/triangle/noise/wavetable.
 * Anti-aliased saw via poly-BLEP-lite (parabolic residual). Sine via
 * phase accumulation. Wavetable via mipmapped linear interpolation
 * (G2 [R075]: octave-stripped FFT tables, pitch-based selection).
 * All ours, no libm dependence beyond sqrt/fabs/sin/cos.
 */

#include <math.h>
#include <string.h>
#include "wbus_dsp.h"
#include "g2_fm_simd.h"   /* for vec_sin_4 (used to generate wavetable) */

#define WT_SIZE 256
#define WT_MASK (WT_SIZE - 1)
#define WT_SCALE (float)WT_SIZE

/* ---- Mipmapped Wavetables (G2 [R075]) ----
 * One table per octave band. Table k has harmonics above Nyquist for that
 * octave removed via FFT→strip→IFFT. At runtime, select table by pitch. */

#define WT_NUM_TABLES 8   /* 8 octave bands of mipmaps */

static float wt_tables[WT_NUM_TABLES][WT_SIZE];
static int wt_initialized = 0;

/* Generate a mipmapped wavetable set from a base sawtooth with rich harmonics.
 * Each table k is band-limited to avoid aliasing at the top of its octave:
 * table 0 = full bandwidth (lowest octave), table 7 = near-sine (highest). */
static void wt_generate_mipmaps(void) {
    int base_size = 2048;  /* high-res for clean FFT */
    double *base_re = (double *)calloc(base_size, sizeof(double));

    /* Generate sawtooth: sum of harmonics 1..N with 1/n amplitude */
    int n_harmonics = base_size / 2 - 1;
    for (int h = 1; h <= n_harmonics; h++) {
        double amp = 1.0 / (double)h;
        if (h % 2 == 0) amp = -amp;  /* alternating sign for sawtooth */
        for (int i = 0; i < base_size; i++) {
            base_re[i] += amp * sin(2.0 * M_PI * (double)h * (double)i / (double)base_size);
        }
    }

    /* Bit-reversal table */
    int bits = 0;
    for (int v = base_size; v > 1; v >>= 1) bits++;
    int *rev = (int *)malloc(base_size * sizeof(int));
    for (int i = 0; i < base_size; i++) {
        int r = 0, x = i;
        for (int b = 0; b < bits; b++) { r = (r << 1) | (x & 1); x >>= 1; }
        rev[i] = r;
    }

    /* Forward FFT: bit-reverse copy + butterfly */
    double *fft_re = (double *)calloc(base_size, sizeof(double));
    double *fft_im = (double *)calloc(base_size, sizeof(double));
    for (int i = 0; i < base_size; i++) {
        fft_re[i] = base_re[rev[i]];
    }
    free(base_re);
    for (int len = 2; len <= base_size; len <<= 1) {
        int half = len >> 1;
        for (int i = 0; i < base_size; i += len) {
            for (int k = 0; k < half; k++) {
                double ang = -2.0 * M_PI * (double)k / (double)len;
                double c = cos(ang), s = sin(ang);
                double tre = c * fft_re[i+k+half] - s * fft_im[i+k+half];
                double wi  = c * fft_im[i+k+half] + s * fft_re[i+k+half];
                fft_re[i+k+half] = fft_re[i+k] - tre;
                fft_im[i+k+half] = fft_im[i+k] - wi;
                fft_re[i+k]       += tre;
                fft_im[i+k]       += wi;
            }
        }
    }

    /* For each mipmap level: copy spectrum, strip harmonics above Nyquist
     * for that octave, IFFT, decimate to WT_SIZE */
    double *tbl_re = (double *)calloc(base_size, sizeof(double));
    double *tbl_im = (double *)calloc(base_size, sizeof(double));
    double *ifft_re = (double *)calloc(base_size, sizeof(double));
    double *ifft_im = (double *)calloc(base_size, sizeof(double));

    for (int tbl = 0; tbl < WT_NUM_TABLES; tbl++) {
        /* Table k preserves harmonics up to base_size/(2^(k+1)) */
        int max_harmonic = base_size >> (tbl + 1);
        if (max_harmonic < 1) max_harmonic = 1;

        memcpy(tbl_re, fft_re, base_size * sizeof(double));
        memcpy(tbl_im, fft_im, base_size * sizeof(double));

        /* Zero out harmonics above max_harmonic */
        for (int h = max_harmonic + 1; h < base_size / 2; h++) {
            tbl_re[h] = 0.0; tbl_im[h] = 0.0;
            tbl_re[base_size - h] = 0.0; tbl_im[base_size - h] = 0.0;
        }

        /* IFFT: bit-reverse copy + inverse butterfly */
        for (int i = 0; i < base_size; i++) {
            ifft_re[i] = tbl_re[rev[i]];
            ifft_im[i] = tbl_im[rev[i]];
        }
        for (int len = 2; len <= base_size; len <<= 1) {
            int half = len >> 1;
            for (int i = 0; i < base_size; i += len) {
                for (int k = 0; k < half; k++) {
                    double ang = 2.0 * M_PI * (double)k / (double)len;
                    double c = cos(ang), s = sin(ang);
                    double tre = c * ifft_re[i+k+half] - s * ifft_im[i+k+half];
                    double wi  = c * ifft_im[i+k+half] + s * ifft_re[i+k+half];
                    ifft_re[i+k+half] = ifft_re[i+k] - tre;
                    ifft_im[i+k+half] = ifft_im[i+k] - wi;
                    ifft_re[i+k]       += tre;
                    ifft_im[i+k]       += wi;
                }
            }
        }

        /* Normalize and decimate to WT_SIZE */
        float peak = 0.0f;
        for (int i = 0; i < WT_SIZE; i++) {
            int src = i * (base_size / WT_SIZE);
            float val = (float)(ifft_re[src] / (double)base_size);
            wt_tables[tbl][i] = val;
            float a = fabsf(val);
            if (a > peak) peak = a;
        }
        /* Normalize to [-1,1] */
        if (peak > 0.001f) {
            for (int i = 0; i < WT_SIZE; i++)
                wt_tables[tbl][i] /= peak;
        }
    }

    free(tbl_re); free(tbl_im);
    free(ifft_re); free(ifft_im);
    free(fft_re); free(fft_im);
    free(rev);
    wt_initialized = 1;
}

/* Select mipmap table index from phase increment (radians/sample).
 * Higher frequency → higher table index (fewer harmonics). */
static inline int wt_select_table(float inc) {
    /* inc = 2π * freq / sr. Table boundary: freq = sr / (2^(tbl+2)) */
    /* So tbl = log2(sr / (4 * freq)) = log2(2π / (4 * inc)) */
    /* Simplified: count leading zeros of inc as a proxy */
    if (inc <= 0.0f) return 0;
    union { float f; uint32_t u; } v = { inc };
    int exponent = (int)((v.u >> 23) & 0xFF) - 127;
    /* Map exponent to table: higher freq (larger exponent) → higher table */
    int tbl = 31 - (-exponent);  /* rough mapping */
    /* Better: use float exponent directly */
    /* inc ≈ 2π*f/44100. For f=20Hz: inc≈0.00285 (exp≈-8) → tbl 0
     * For f=20kHz: inc≈2.85 (exp≈1) → tbl 7 */
    tbl = (exponent + 8) / 2;
    if (tbl < 0) tbl = 0;
    if (tbl >= WT_NUM_TABLES) tbl = WT_NUM_TABLES - 1;
    return tbl;
}

static void wt_init(void) {
    if (!wt_initialized) {
        wt_generate_mipmaps();
        wt_initialized = 1;
    }
}

/* ---- SIMD wavetable: sample 4 oscillator phases from a mipmapped table ---- */
/* All 4 oscillators use the same mipmap table (same inc). For per-oscillator
 * table selection, the scalar path is used. The SIMD path assumes unison. */
static inline __m128 wt_sample_4(__m128 phase_vec, float inc) {
    wt_init();
    /* phase_vec is in [0, 2π). Convert to [0, 256) table index. */
    __m128 scale = _mm_set1_ps(WT_SCALE / (float)(2.0 * M_PI));
    __m128 idx_f = _mm_mul_ps(phase_vec, scale);

    /* Integer index and fractional part for linear interpolation */
    __m128i idx0_i = _mm_cvttps_epi32(idx_f);
    __m128 frac = _mm_sub_ps(idx_f, _mm_cvtepi32_ps(idx0_i));

    /* Wrap indices to [0, 255] */
    __m128i mask = _mm_set1_epi32(WT_MASK);
    idx0_i = _mm_and_si128(idx0_i, mask);
    __m128i idx1_i = _mm_and_si128(_mm_add_epi32(idx0_i, _mm_set1_epi32(1)), mask);

    /* Select mipmap table */
    int tbl = wt_select_table(inc);
    float *table = wt_tables[tbl];

    /* Gather (scalar gather — load 4 individual entries) */
    int idx0_arr[4], idx1_arr[4];
    _mm_storeu_si128((__m128i*)idx0_arr, idx0_i);
    _mm_storeu_si128((__m128i*)idx1_arr, idx1_i);

    __m128 y0 = _mm_setr_ps(table[idx0_arr[0]], table[idx0_arr[1]],
                              table[idx0_arr[2]], table[idx0_arr[3]]);
    __m128 y1 = _mm_setr_ps(table[idx1_arr[0]], table[idx1_arr[1]],
                              table[idx1_arr[2]], table[idx1_arr[3]]);

    /* Linear interpolation: y = y0 + frac * (y1 - y0) */
    return _mm_add_ps(y0, _mm_mul_ps(frac, _mm_sub_ps(y1, y0)));
}

void wb_osc_reset(wb_osc *o) {
    o->phase = 0.0;
    o->phase_inc = 0.0;
    o->last_out = 0.0;
}

/* poly-BLEP residual for band-limited edges */
static float blep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

float wb_osc_process(wb_osc *o, float inc, int waveform, float shape) {
    o->phase += inc;
    if (o->phase >= 2.0 * M_PI) o->phase -= 2.0 * M_PI;
    if (o->phase < 0) o->phase += 2.0 * M_PI;

    float t = (float)(o->phase / (2.0 * M_PI));   /* 0..1 */
    float dt = inc / (float)(2.0 * M_PI);
    float out = 0.0f;

    switch (waveform) {
    case WB_WAVE_SINE:
        out = (float)sin(o->phase);
        break;
    case WB_WAVE_SAW: {
        out = 2.0f * t - 1.0f;                 /* naive saw */
        out -= blep(t, dt);                    /* poly-BLEP correction */
        /* second edge at t=1 (the wrap) handled implicitly */
        break;
    }
    case WB_WAVE_SQUARE: {
        float pw = (shape < 0.01f) ? 0.5f : shape; /* pulse width 0..1 */
        out = (t < pw) ? 1.0f : -1.0f;
        out += blep(t, dt);
        out -= blep(fmodf(t + 1.0f - pw, 1.0f), dt);
        break;
    }
    case WB_WAVE_TRIANGLE: {
        out = 4.0f * fabsf(t - 0.5f) - 1.0f;
        out = out * 0.5f + 0.5f;               /* normalise 0..1 */
        break;
    }
    case WB_WAVE_NOISE:
        out = wb_noise_next();
        break;
    case WB_WAVE_WAVETABLE: {
        wt_init();
        int tbl = wt_select_table(inc);
        float *table = wt_tables[tbl];
        float idx_f = (float)(o->phase / (2.0 * M_PI)) * WT_SIZE;
        int idx0 = (int)idx_f & WT_MASK;
        int idx1 = (idx0 + 1) & WT_MASK;
        float frac = idx_f - (float)(int)idx_f;
        out = table[idx0] + frac * (table[idx1] - table[idx0]);
        break;
    }
    default:
        out = 0.0f;
        break;
    }
    o->last_out = out;
    return out;
}

/* ---- SIMD batch wavetable: process 4 oscillators at once ---- */
/* Used by the synth SIMD path for WB_WAVE_WAVETABLE */
__m128 wb_osc_process_wt4(__m128 *phases, float inc) {
    wt_init();
    /* Advance 4 phases */
    __m128 inc_vec = _mm_set1_ps(inc);
    __m128 new_phase = _mm_add_ps(*phases, inc_vec);
    /* Wrap to [0, 2π) */
    __m128 two_pi = _mm_set1_ps((float)(2.0 * M_PI));
    __m128 ge_mask = _mm_cmpge_ps(new_phase, two_pi);
    new_phase = _mm_sub_ps(new_phase, _mm_and_ps(ge_mask, two_pi));
    *phases = new_phase;
    /* Sample wavetable (mipmap selected by inc) */
    return wt_sample_4(new_phase, inc);
}

static unsigned long rng_state = 0x9E3779B9u;

void wb_noise_seed(unsigned long s) { rng_state = s ? s : 0x9E3779B9u; }

float wb_noise_next(void) {
    /* xorshift32 */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (float)((double)(rng_state & 0xFFFFFF) / 8388608.0 - 1.0);
}
