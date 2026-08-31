/* wb_conv_reverb.c — hybrid convolution + algorithmic FDN reverb.
 *
 * Ableton Hybrid Reverb style: an FFT-based overlap-save convolutional
 * engine (impulse-response reverb) blended with an 8x8 feedback delay
 * network (algorithmic tail). Both paths run in parallel and are
 * mixed via the dry/wet parameter; toggling algorithmic mode routes
 * the dry signal into the FDN instead of (or in addition to) the
 * convolutional IR path.
 *
 * Pure C11, zero third-party. Uses wb_fft.c (radix-2 Cooley-Tukey).
 *
 * G1 [R075]: convolution reverb module (hybrid tail).
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "wbus.h"
#include "wbus_fft.h"

/* ---- config --------------------------------------------------------- */

#define CV_IR_FFT_BITS  15          /* 32768-bin FFT for IR spectrum */
#define CV_IR_FFT_SIZE  (1 << CV_IR_FFT_BITS)
#define CV_BLOCK_SIZE   512         /* processing block (samples, mono) */
#define CV_N_DELAYS     8          /* FDN order */
#define CV_N_TAPS       8          /* output taps per delay (matrix size) */

/* ---- FDN delay lengths (prime-ish, stereo-spread) ------------------- */

static const int FDN_DELAYS[CV_N_DELAYS] = {
    1557, 1553, 1549, 1543,
    1537, 1529, 1517, 1513
};

/* Householder matrix coefficients (4x4 Hadamard-style, normalized).
 * We apply a 4x4 Hadamard to the 8 delay outputs in two groups of 4,
 * plus a transpose-style cross-feed that widens the stereo field. */

/* ---- pre-delay ring buffer ------------------------------------------ */

typedef struct {
    float *buf;
    uint32_t size;      /* in samples */
    uint32_t pos;
} cv_predelay;

static void cv_predelay_init(cv_predelay *p, uint32_t max_samples) {
    p->size = max_samples;
    if (p->size < 1) p->size = 1;
    p->buf = (float *)calloc(p->size, sizeof(float));
    p->pos = 0;
}

static void cv_predelay_destroy(cv_predelay *p) {
    free(p->buf);
    p->buf = NULL;
    p->size = 0;
    p->pos = 0;
}

/* Set delay length (reallocates). */
static void cv_predelay_set(cv_predelay *p, uint32_t samples) {
    if (!samples) samples = 1;
    float *newbuf = (float *)calloc(samples, sizeof(float));
    if (newbuf) {
        /* copy old content preserving order */
        uint32_t old = p->pos;
        uint32_t n = p->size < samples ? p->size : samples;
        for (uint32_t i = 0; i < n; i++)
            newbuf[(samples - n + i) % samples] = p->buf[(old + i) % p->size];
        free(p->buf);
        p->buf = newbuf;
        p->size = samples;
        p->pos = (samples - n) % samples;
    }
}

/* Write sample, return delayed sample (read pointer = write - size). */
static float cv_predelay_write(cv_predelay *p, float in) {
    p->pos = (p->pos + 1) % p->size;
    p->buf[p->pos] = in;
    uint32_t read = (p->pos + (p->size - 1)) % p->size;
    return p->buf[read];
}

/* ---- overlap-save FFT convolution ----------------------------------- */

typedef struct {
    double *re;
    double *im;
} cv_spectrum;

static int cv_next_pow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* ---- main instance -------------------------------------------------- */

struct wb_conv_reverb {
    uint32_t sr;

    /* ---- convolution path ---- */
    wb_fft_plan *plan;
    cv_spectrum ir_spectrum;        /* pre-computed IR FFT (CV_IR_FFT_SIZE bins) */
    uint32_t ir_len;                /* original IR length in samples */
    uint32_t ir_loaded;             /* 1 if an IR has been loaded */
    double *conv_input;             /* ring/accum for overlap-save */
    double *conv_out;               /* current block IFFT output */
    uint32_t conv_pos;              /* write cursor in conv_input */
    uint32_t conv_n;                /* = CV_IR_N_DELAYS? no — fft size */

    /* ---- FDN path ---- */
    float *fdn_delay[CV_N_DELAYS];  /* per-delay line buffer   */
    uint32_t fdn_len[CV_N_DELAYS];  /* delay line length (samples) */
    uint32_t fdn_pos[CV_N_DELAYS];  /* write cursor per delay */
    float fdn_gain[CV_N_DELAYS];    /* feedback gain per delay */
    float fdn_decay;                /* target RT60 in seconds */
    float fdn_size;                 /* size multiplier 0.5..2.0 */

    /* ---- pre-delay ---- */
    cv_predelay predelay;
    float predelay_ms;

    /* ---- mix / mode ---- */
    float dry_wet;                  /* 0 = dry, 1 = wet */
    int algorithmic;                /* 1 = FDN-only (bypasses convolution) */

    /* output energy tracking for tests */
    float peak_out;
};

/* ---- public API ---- */

void *wb_conv_reverb_create(uint32_t sr) {
    struct wb_conv_reverb *r = (struct wb_conv_reverb *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->sr = sr;

    /* FFT plan for overlap-save (power of two) */
    r->plan = wb_fft_create(CV_IR_FFT_SIZE);
    if (!r->plan) { free(r); return NULL; }

    /* Convolution state: input ring + output accum */
    r->conv_input = (double *)calloc(CV_IR_FFT_SIZE, sizeof(double));
    r->conv_out   = (double *)calloc(CV_IR_FFT_SIZE, sizeof(double));
    if (!r->conv_input || !r->conv_out) {
        wb_fft_destroy(r->plan);
        free(r->conv_input);
        free(r->conv_out);
        free(r);
        return NULL;
    }
    r->ir_spectrum.re = (double *)calloc(CV_IR_FFT_SIZE, sizeof(double));
    r->ir_spectrum.im = (double *)calloc(CV_IR_FFT_SIZE, sizeof(double));
    if (!r->ir_spectrum.re || !r->ir_spectrum.im) {
        wb_fft_destroy(r->plan);
        free(r->conv_input); free(r->conv_out);
        free(r->ir_spectrum.re); free(r->ir_spectrum.im);
        free(r); return NULL;
    }

    /* FDN default: RT60 2.5s at 48kHz, scale for sr */
    r->fdn_decay = 2.5f;
    r->fdn_size = 1.0f;

    /* Pre-delay default 15ms */
    r->predelay_ms = 15.0f;
    uint32_t pd_samp = (uint32_t)(r->predelay_ms * 0.001f * sr);
    cv_predelay_init(&r->predelay, pd_samp > 0 ? pd_samp : 1);

    /* Initialize FDN delay lines */
    for (int i = 0; i < CV_N_DELAYS; i++) {
        uint32_t base = (uint32_t)(FDN_DELAYS[i] * r->fdn_size);
        if (base < 64) base = 64;
        r->fdn_len[i] = base;
        r->fdn_delay[i] = (float *)calloc(base, sizeof(float));
        r->fdn_pos[i] = 0;
        r->fdn_gain[i] = 0.5f; /* default; recomputed from RT60 */
    }
    /* Compute feedback gains from RT60 */
    float target_energy = powf(10.0f, -3.0f); /* RT60 = -60 dB */
    for (int i = 0; i < CV_N_DELAYS; i++) {
        /* g = exp(-ln(1/target_energy) * delay_len / (sr * decay)) */
        float d = (float)r->fdn_len[i];
        float exponent = -logf(target_energy) * d / ((float)sr * r->fdn_decay);
        if (exponent > 20.0f) exponent = 20.0f;  /* clamp */
        r->fdn_gain[i] = expf(-exponent);
        if (r->fdn_gain[i] > 0.999f) r->fdn_gain[i] = 0.999f;
        if (r->fdn_gain[i] < 0.001f) r->fdn_gain[i] = 0.001f;
    }

    r->dry_wet = 0.5f;
    r->algorithmic = 0;
    r->ir_loaded = 0;
    r->peak_out = 0.0f;

    return r;
}

void wb_conv_reverb_destroy(void *ptr) {
    struct wb_conv_reverb *r = (struct wb_conv_reverb *)ptr;
    if (!r) return;

    if (r->plan) wb_fft_destroy(r->plan);
    free(r->ir_spectrum.re);
    free(r->ir_spectrum.im);
    free(r->conv_input);
    free(r->conv_out);
    cv_predelay_destroy(&r->predelay);

    for (int i = 0; i < CV_N_DELAYS; i++) {
        free(r->fdn_delay[i]);
    }
    free(r);
}

/* Load an impulse response (mono). Computes the IR's FFT spectrum
 * for overlap-save fast convolution. Returns 0 on success, negative
 * on error. */
int wb_conv_reverb_load_ir(void *ptr, const wb_sample *ir, uint32_t frames, uint3_t_chn_unused) {
    (void)chn;  /* IR is always mono in this simple path */
    struct wb_conv_reverb *r = (struct wb_conv_reverb *)ptr;
    if (!r || !ir || frames < 4) return -1;

    /* Zero-pad IR to FFT size and FFT it */
    float *ir_padded = (float *)calloc(CV_IR_FFT_SIZE, sizeof(float));
    if (!ir_padded) return -3;
    uint32_t copy_len = frames < CV_IR_FFT_SIZE ? frames : CV_IR_FFT_SIZE;
    for (uint32_t i = 0; i < copy_len; i++)
        ir_padded[i] = ir[i];

    double *ire = (double *)calloc(CV_IR_FFT_SIZE, sizeof(double));
    double *imi = (double *)calloc(CV_IR_FFT_SIZE, sizeof(double));
    if (!ire || !imi) {
        free(ir_padded); free(ire); free(imi); return -3;
    }
    for (uint32_t i = 0; i < CV_IR_FFT_SIZE; i++) {
        ire[i] = (double)ir_padded[i];
    }
    wb_fft_run(r->plan, ire, imi, 0);

    /* Store the spectrum */
    memcpy(r->ir_spectrum.re, ire, CV_IR_FFT_SIZE * sizeof(double));
    memcpy(r->ir_spectrum.im, imi, CV_IR_FFT_SIZE * sizeof(double));
    r->ir_len = frames;
    r->ir_loaded = 1;

    free(ir_padded); free(ire); free(imi);
    return 0;
}
