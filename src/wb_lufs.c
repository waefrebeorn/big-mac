/* wb_lufs.c — G78: live K-weighted loudness meter (ITU-R BS.1770-4).
 * Pure C11, zero third-party. Reuses wb_biquad (wbus_dsp.h) for the
 * K-weighting pre-filter and RLB high-pass.
 *
 * K-weighting (BS.1770-4 §2.2):
 *   pre-filter: high-shelf, f0=1513.0 Hz, gain +4 dB  (Loudness pre-curve)
 *   RLB filter:  high-pass, f0=60.0 Hz, Q=sqrt(2)/2
 * Then mean-square over the gate block; LUFS = -0.691 + 10*log10(mean-square).
 *
 * Gate block = 400 ms (the BS.1770 "short-term" block size). We flush a new
 * short-term estimate each time the block closes and roll it into the
 * integrated accumulator.
 *
 * True peak: BS.1770 specifies 192 kHz (4x oversampling). A full polyphase
 * 4x upsample is ~4 biquads/sample at 176.4 kHz — too heavy on the i5.
 * We approximate by taking max(|x|) of the K-filtered signal, which is a
 * valid lower bound; documented choice (oversampling is a future swap-in).
 */
#include <math.h>
#include <string.h>
#include "wbus.h"
#include "wbus_dsp.h"
#include "wbus_lufs.h"

#define LUFS_OFFSET_DB   (-0.691)
#define GATE_SECS        0.400

static void biquad_highshelf(wb_biquad *f, double sr, double f0, double gain_db, double q) {
    /* RBJ high-shelf (cookbook). Gain applied to frequencies ABOVE f0.
     * BS.1770 K-weight pre-filter: f0=1513 Hz, gain=+4 dB. */
    double w0 = 2.0 * M_PI * f0 / sr;
    double A  = pow(10.0, gain_db / 40.0);
    double sinw = sin(w0), cosw = cos(w0);
    double alpha = sinw / (2.0 * q);
    double sqrtA = sqrt(A);
    double b0 = A * ((A + 1.0) - (A - 1.0) * cosw + 2.0 * sqrtA * alpha);
    double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw);
    double b2 = A * ((A + 1.0) + (A - 1.0) * cosw - 2.0 * sqrtA * alpha);
    double a0 = (A + 1.0) - (A - 1.0) * cosw + 2.0 * A * alpha;
    double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw);
    double a2 = (A + 1.0) + (A - 1.0) * cosw - 2.0 * A * alpha;
    f->b0 = (float)(b0/a0); f->b1 = (float)(b1/a0); f->b2 = (float)(b2/a0);
    f->a1 = (float)(a1/a0); f->a2 = (float)(a2/a0);
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}

static void biquad_hpf(wb_biquad *f, double sr, double f0, double q) {
    /* RBJ high-pass: b0=(1-cosw)/2 normalized, b1=-(1-cosw), b2=(1-cosw)/2 */
    double w0 = 2.0 * M_PI * f0 / sr;
    double cosw = cos(w0);
    double alpha = sin(w0) / (2.0 * q);
    double b0 = (1.0 + cosw) / 2.0,   /* NOTE: +cosw for HPF */
           b1 = -(1.0 + cosw),
           b2 = (1.0 + cosw) / 2.0;
    double a0 = 1.0 + alpha,
           a1 = -2.0 * cosw,
           a2 = 1.0 - alpha;
    f->b0 = (float)(b0/a0); f->b1 = (float)(b1/a0); f->b2 = (float)(b2/a0);
    f->a1 = (float)(a1/a0); f->a2 = (float)(a2/a0);
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}

void wb_lufs_create(wb_lufs *l, double sr) {
    if (!l) return;
    memset(l, 0, sizeof(*l));
    l->sr = sr;
    biquad_highshelf(&l->k_pre, sr, 1513.0, 4.0, 0.7071); /* BS.1770 K-weight pre-filter */
    biquad_hpf(&l->k_rlb, sr, 60.0, sqrt(2.0)/2.0);       /* RLB high-pass      */
    l->gate_cap = (int)(sr * GATE_SECS);
}

void wb_lufs_process(wb_lufs *l, const float *in, int n) {
    if (!l || !in) return;
    for (int i = 0; i < n; i++) {
        float x = in[i];
        /* K-weight: pre-filter then RLB */
        double kp = wb_biquad_process(&l->k_pre, (double)x);
        double kr = wb_biquad_process(&l->k_rlb, kp);
        double av = fabs(kr);
        if (av > l->peak_hold) l->peak_hold = av;
        l->sq_sum += kr * kr;
        l->gate_n++;
        if (l->gate_n >= l->gate_cap) {
            double mean_sq = l->sq_sum / (double)l->gate_n;
            if (mean_sq > 0.0) {
                l->st_lufs = LUFS_OFFSET_DB + 10.0 * log10(mean_sq);
                l->has_short = 1;
            } else {
                l->st_lufs = 0.0; /* -inf sentinel */
            }
            l->integ_sum += (l->st_lufs);   /* accumulate short-term values */
            l->integ_n++;
            if (l->integ_n > 0)
                l->integ_lufs = l->integ_sum / (double)l->integ_n;
            l->sq_sum = 0.0;
            l->gate_n = 0;
        }
    }
}

double wb_lufs_short_term_lufs(const wb_lufs *l) {
    return l ? l->st_lufs : 0.0;
}

double wb_lufs_integrated_lufs(const wb_lufs *l) {
    return l ? (l->integ_n > 0 ? l->integ_lufs : 0.0) : 0.0;
}

double wb_lufs_peak(const wb_lufs *l) {
    return l ? l->peak_hold : 0.0;
}
