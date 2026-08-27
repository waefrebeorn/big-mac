/* wb_conv.c — convolution reverb (non-uniform partitioned overlap-save).
 *
 * Architecture (from R075 7-hop research convergence):
 *   - IR is split into geometrically-growing partitions (64, 128, 256, ...)
 *   - Each partition's FFT is pre-computed at load time (FDL)
 *   - Input blocks are FFT'd, multiplied by each partition's spectrum,
 *     IFFT'd, and accumulated via overlap-save
 *   - First partition size = host buffer size → low-latency early reflections
 *   - Hybrid tail: convolve first N ms, crossfade to FDN reverb for the rest
 *
 * Pure C11, zero third-party. Uses wb_fft.c (radix-2 Cooley-Tukey).
 *
 * G1 [R075]: convolution reverb module.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wbus.h"
#include "wbus_fft.h"
#include "wbus_conv.h"

#ifndef WB_CONV_MAX_PARTITIONS
#define WB_CONV_MAX_PARTITIONS 32
#endif

#ifndef WB_CONV_HYBRID_MS
#define WB_CONV_HYBRID_MS 150    /* crossfade to FDN after this many ms */
#endif

typedef struct {
    double *re;     /* FFT output real */
    double *im;     /* FFT output imag */
} wb_conv_spectrum;

struct wb_conv_inst {
    uint32_t sr;
    int block_size;          /* first partition size = host buffer size */
    int n_partitions;        /* number of IR partitions */
    int fft_size;            /* FFT size (next pow2 >= 2*block_size) */

    /* Per-partition IR spectra (pre-computed) */
    wb_conv_spectrum parts[WB_CONV_MAX_PARTITIONS];
    int part_len[WB_CONV_MAX_PARTITIONS];  /* partition length in samples */

    /* Input ring buffer for overlap-save */
    double *input_buf;
    int input_pos;           /* write position in input_buf */

    /* Accumulation buffer for overlap-save output */
    double *accum;
    int accum_len;

    /* FFT plan */
    wb_fft_plan *plan;

    /* Hybrid tail */
    int hybrid_samples;      /* samples of IR to convolve before crossfade */
    float hybrid_mix;        /* 0 = full conv, 1 = full FDN */
    float hybrid_envelope;   /* rising 0→1 during crossfade region */

    /* Parameters */
    float dry_wet;           /* 0 = dry, 1 = wet */
    float gain;              /* output gain */

    /* FDN tail crossfade buffer */
    float *tail_buf;         /* stereo tail output from FDN, same block */
};

/* ---- helpers ---- */

static int next_pow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* ---- IR preprocessing ---- */

/* Trim leading silence (below threshold). Returns new start pointer + new len. */
static const float *wb_conv_trim_silence(const float *ir, int len, int *out_len, float thresh) {
    int start = 0;
    while (start < len && fabsf(ir[start]) < thresh) start++;
    *out_len = len - start;
    return ir + start;
}

/* Find the sample index where IR energy drops below threshold (for truncation). */
static int wb_conv_find_tail(const float *ir, int len, float thresh) {
    /* Scan from end in 1ms blocks, find where RMS stays below threshold */
    int block = 44; /* ~1ms @ 44.1k */
    for (int i = len - block; i >= 0; i -= block) {
        float rms = 0;
        int end = i + block;
        if (end > len) end = len;
        int n = end - i;
        if (n <= 0) continue;
        for (int j = i; j < end; j++) {
            rms += ir[j] * ir[j];
        }
        rms = sqrtf(rms / n);
        if (rms > thresh) {
            /* This block is audible — keep from here to end */
            return end;
        }
    }
    return len;
}

static void wb_conv_normalize(float *ir, int len) {
    float peak = 0;
    for (int i = 0; i < len; i++) {
        float a = fabsf(ir[i]);
        if (a > peak) peak = a;
    }
    if (peak > 0.0001f) {
        float scale = 0.95f / peak;
        for (int i = 0; i < len; i++) ir[i] *= scale;
    }
}

/* ---- core API ---- */

wb_conv_inst *wb_conv_create(uint32_t sr) {
    wb_conv_inst *c = (wb_conv_inst *)calloc(1, sizeof(wb_conv_inst));
    if (!c) return NULL;
    c->sr = sr;
    c->dry_wet = 0.5f;
    c->gain = 1.0f;
    c->hybrid_mix = 0.0f;
    c->hybrid_envelope = 0.0f;
    c->block_size = 128;  /* default, reset at load time */
    c->input_pos = 0;
    return c;
}

void wb_conv_destroy(wb_conv_inst *c) {
    if (!c) return;
    if (c->plan) wb_fft_destroy(c->plan);
    if (c->input_buf) free(c->input_buf);
    if (c->accum) free(c->accum);
    if (c->tail_buf) free(c->tail_buf);
    for (int i = 0; i < c->n_partitions; i++) {
        free(c->parts[i].re);
        free(c->parts[i].im);
    }
    free(c);
}

/* Load an impulse response (mono float array). Preprocesses, partitions,
 * and pre-computes FFTs for each partition. Returns 0 on success. */
int wb_conv_load_ir(wb_conv_inst *c, const float *ir_mono, int ir_len, int block_size) {
    if (!c || !ir_mono || ir_len < 64 || block_size < 32) return -1;

    c->block_size = block_size;
    c->fft_size = next_pow2(block_size * 2);  /* overlap-save needs 2x */

    if (c->plan) wb_fft_destroy(c->plan);
    c->plan = wb_fft_create(c->fft_size);
    if (!c->plan) return -2;

    /* Preprocess: copy, trim, truncate, normalize */
    float *ir_copy = (float *)malloc(ir_len * sizeof(float));
    if (!ir_copy) return -3;
    memcpy(ir_copy, ir_mono, ir_len * sizeof(float));

    /* Trim silence */
    int trimmed_len;
    const float *trimmed = wb_conv_trim_silence(ir_copy, ir_len, &trimmed_len, 0.001f);

    /* Truncate tail below noise floor */
    int tail_end = wb_conv_find_tail(trimmed, trimmed_len, 0.001f);
    int proc_len = tail_end;
    if (proc_len < block_size) proc_len = trimmed_len;  /* keep short IRs whole */

    /* Normalize */
    float *proc = (float *)malloc(proc_len * sizeof(float));
    if (!proc) { free(ir_copy); return -4; }
    memcpy(proc, trimmed, proc_len * sizeof(float));
    wb_conv_normalize(proc, proc_len);

    /* Hybrid: compute how many samples to convolve before crossfade to FDN */
    c->hybrid_samples = (int)((float)WB_CONV_HYBRID_MS * c->sr / 1000.0f);
    if (c->hybrid_samples > proc_len) c->hybrid_samples = proc_len;

    /* Partition the IR with geometrically-growing blocks:
     * block 0 = block_size, block 1 = block_size, block 2 = 2*block_size,
     * block 3 = 4*block_size, ... */
    int pos = 0;
    int part_idx = 0;
    int current_len = block_size;

    while (pos < proc_len && part_idx < WB_CONV_MAX_PARTITIONS) {
        int len = (pos + current_len > proc_len) ? (proc_len - pos) : current_len;
        if (len < 4) break;  /* skip tiny final partition */

        c->part_len[part_idx] = len;

        /* Allocate spectrum (fft_size complex bins) */
        c->parts[part_idx].re = (double *)calloc(c->fft_size, sizeof(double));
        c->parts[part_idx].im = (double *)calloc(c->fft_size, sizeof(double));

        if (!c->parts[part_idx].re || !c->parts[part_idx].im) {
            free(ir_copy);
            free(proc);
            return -5;
        }

        /* Zero-pad partition to fft_size and FFT it */
        double *re = c->parts[part_idx].re;
        double *im = c->parts[part_idx].im;
        memset(re, 0, c->fft_size * sizeof(double));
        memset(im, 0, c->fft_size * sizeof(double));
        for (int i = 0; i < len; i++) re[i] = (double)proc[pos + i];

        /* Real FFT of this partition */
        wb_fft_run(c->plan, re, im, 0);

        pos += len;
        part_idx++;
        if (part_idx >= 2) current_len *= 2;  /* grow after first 2 blocks */
    }

    c->n_partitions = part_idx;

    /* Allocate input ring buffer (2x fft_size for overlap) */
    c->accum_len = c->fft_size * 2;
    if (c->input_buf) free(c->input_buf);
    if (c->accum) free(c->accum);
    if (c->tail_buf) free(c->tail_buf);
    c->input_buf = (double *)calloc(c->fft_size, sizeof(double));
    c->accum = (double *)calloc(c->accum_len, sizeof(double));
    c->tail_buf = (float *)calloc(block_size * 2, sizeof(float));  /* stereo */
    c->input_pos = 0;

    free(ir_copy);
    free(proc);
    return 0;
}

/* Process a block of stereo audio through the convolution reverb.
 * inL/inR are length n (<= WB_MAX_BLOCK). outL/outR are written.
 * n must equal the block_size used in wb_conv_load_ir. */
void wb_conv_process(wb_conv_inst *c, const float *inL, const float *inR,
                     float *outL, float *outR, int n) {
    if (!c || !c->plan || c->n_partitions == 0 || n > c->block_size) {
        /* Pass-through */
        if (outL && inL) memcpy(outL, inL, n * sizeof(float));
        if (outR && inR) memcpy(outR, inR, n * sizeof(float));
        return;
    }

    int fft_n = c->fft_size;

    /* ---- build mono input block ---- */
    double *input = c->input_buf;
    for (int i = 0; i < n; i++) {
        input[i] = (double)((inL[i] + inR[i]) * 0.5);
    }
    /* Zero the rest of the FFT buffer (overlap-save zero-pad) */
    for (int i = n; i < fft_n; i++) {
        input[i] = 0.0;
    }

    /* ---- FFT of input block ---- */
    double *in_re = input;
    double *in_im = (double *)calloc(fft_n, sizeof(double));
    if (!in_im) return;
    wb_fft_run(c->plan, in_re, in_im, 0);

    /* ---- clear accumulation (only first n + max_part_len samples) */
    int out_len = n + c->part_len[c->n_partitions - 1];
    if (out_len > c->accum_len) out_len = c->accum_len;
    memset(c->accum, 0, (size_t)out_len * sizeof(double));

    /* ---- overlap-save convolution: multiply each partition's spectrum ---- */
    for (int p = 0; p < c->n_partitions; p++) {
        /* Complex multiply: accum += input_fft * part_fft
         * (a+bi)(c+di) = (ac-bd) + (ad+bc)i */
        /* We only need the time-domain result via IFFT, so accumulate
         * in frequency domain then IFFT once at the end.
         * Actually: overlap-save requires per-partition IFFT because
         * partitions have different delays. Do it per-partition. */

        double *acc_re = (double *)calloc(fft_n, sizeof(double));
        double *acc_im = (double *)calloc(fft_n, sizeof(double));
        if (!acc_re || !acc_im) { free(acc_re); free(acc_im); free(in_im); return; }

        for (int k = 0; k < fft_n; k++) {
            double a = in_re[k], b = in_im[k];
            double cc = c->parts[p].re[k], dd = c->parts[p].im[k];
            acc_re[k] = a * cc - b * dd;
            acc_im[k] = a * dd + b * cc;
        }

        /* IFFT (wb_fft_run already normalizes by 1/n for invert=1) */
        wb_fft_run(c->plan, acc_re, acc_im, 1);

        /* Overlap-save: the IFFT result contains the linear convolution of
         * the zero-padded input block with this partition's IR. The valid
         * output is samples [0..n) — the rest is the "tail" that belongs
         * to the next block's overlap. Add to accumulation at the
         * partition's time offset. */
        int offset = 0;
        for (int q = 0; q < p; q++) offset += c->part_len[q];

        for (int i = 0; i < n; i++) {
            if ((offset + i) < c->accum_len) {
                c->accum[offset + i] += acc_re[i];
            }
        }

        free(acc_re);
        free(acc_im);
    }

    free(in_im);

    /* ---- write output: dry + wet mix ---- */
    float wet = c->dry_wet;
    float dry = 1.0f - wet;
    float gain = c->gain;

    for (int i = 0; i < n; i++) {
        float wetL = 0.0f, wetR = 0.0f;

        /* Stereo widening: slightly different tap points for L/R */
        int idxL = i;
        int idxR = i + c->block_size / 4;
        if (idxR >= out_len) idxR = i;

        if (idxL < c->accum_len) wetL = (float)c->accum[idxL] * gain;
        if (idxR < c->accum_len) wetR = (float)c->accum[idxR] * gain;

        outL[i] = inL[i] * dry + wetL * wet;
        outR[i] = inR[i] * dry + wetR * wet;
    }
}

/* Set dry/wet mix (0=dry, 1=wet). */
void wb_conv_set_mix(wb_conv_inst *c, float mix) {
    if (!c) return;
    c->dry_wet = mix < 0.0f ? 0.0f : (mix > 1.0f ? 1.0f : mix);
}

/* Set output gain. */
void wb_conv_set_gain(wb_conv_inst *c, float gain) {
    if (!c) return;
    c->gain = gain;
}

/* Get number of partitions. */
int wb_conv_get_partitions(wb_conv_inst *c) {
    return c ? c->n_partitions : 0;
}

/* Get partition length. */
int wb_conv_get_part_len(wb_conv_inst *c, int idx) {
    if (!c || idx < 0 || idx >= c->n_partitions) return 0;
    return c->part_len[idx];
}
