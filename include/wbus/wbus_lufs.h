#ifndef WUBUS_WBUS_LUFS_H
#define WUBUS_WBUS_LUFS_H

/* wb_lufs.h — G78: live K-weighted loudness (ITU-R BS.1770-4 short-term LUFS)
 * + true-peak hold, streamed sample-by-sample / block-by-block.
 * Pure C11, no third-party. Reuses wb_biquad (wbus_dsp.h) for the K-filter.
 *
 * Design (dual-core-i5 budget): the K-weighting filter runs at full rate on
 * EACH sample (cheap: 2 biquads), but the gated mean-square is computed over
 * a rolling 3-second window with 75% overlap — i.e. we emit a new short-term
 * LUFS estimate every 400 ms (the BS.1770 gate block). That is the canonical
 * short-term loudness curve without needing an FFT or full histogram.
 *
 * True peak is a 4x-upsampled peak estimate (interpolation via 2x bilinear
 * half-band + 2x half-band in wb_lufs_process, documented choice to stay
 * allocation-free). On this hardware the 4x oversample is approximated by
 * taking the per-sample peak across the K-filtered output — the BS.1770
 * true-peak spec calls for 192 kHz, we document the simplification.
 */

#include "wbus_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* K-weighting biquad types — reuse wb_biquad with these params. */
#define WB_LUFS_SR_DEFAULT 44100.0

typedef struct wb_lufs {
    double sr;
    wb_biquad k_pre;   /* high-shelf @ ~1.5 kHz, +4 dB (BS.1770) */
    wb_biquad k_rlb;   /* high-pass @ ~60 Hz (RLB)                */
    double   sq_sum;   /* mean-square accumulator in current gate */
    double   peak_hold;/* 0 dBFS-relative peak (K-filtered)      */
    int      gate_n;   /* samples accumulated in this gate block  */
    int      gate_cap; /* samples per 400ms gate at sr            */
    double   st_lufs;  /* last short-term LUFS (-inf => 0.0 if unset) */
    int      has_short;/* 1 once the first gate block closes      */
    double   integ_sum;/* running sum for integrated loudness     */
    int      integ_n;  /* running count of gate blocks             */
    double   integ_lufs;/* integrated LUFS (all blocks)            */
} wb_lufs;

void  wb_lufs_create(wb_lufs *l, double sr);   /* init K-filters */
void  wb_lufs_process(wb_lufs *l, const float *in, int n); /* interleaved stereo or mono */
double wb_lufs_short_term_lufs(const wb_lufs *l);  /* -23..-8 || 0.0 if no data */
double wb_lufs_integrated_lufs(const wb_lufs *l);  /* -23..-8 || 0.0 if no data */
double wb_lufs_peak(const wb_lufs *l);             /* 0..1.0 linear amplitude */

#ifdef __cplusplus
}
#endif
#endif
