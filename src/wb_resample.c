/*
 * wb_resample.c — windowed-sinc (Kaiser) resampler (strict C11).
 * Pulled from wuburvc/src/wubu_audioio.c (WaefreBeorn-UMV3), unmodified
 * except the rename. 64 taps, anti-alias cutoff, beta 14.8.
 */
#include "wb_resample.h"

#include <math.h>
#include <string.h>

#define WUBU_SINC_TAPS 64

static double kaiser_bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; k++) {
        term *= (x * x) / (4.0 * k * k);
        sum += term;
        if (term < 1e-18) break;
    }
    return sum;
}

int wb_resample_sinc(const float *in, int n, int in_sr, int out_sr, float *out) {
    if (!in || !out || n <= 0 || in_sr <= 0 || out_sr <= 0) return -1;
    if (in_sr == out_sr) {
        memcpy(out, in, (size_t)n * sizeof(float));
        return n;
    }
    double ratio = (double)out_sr / (double)in_sr;
    int n_out = (int)(n * ratio);
    if (n_out < 1) n_out = 1;
    const double beta = 14.8;
    const double i0beta = kaiser_bessel_i0(beta);
    const int M = WUBU_SINC_TAPS;
    const double half = M / 2.0;
    const double cutoff = (ratio < 1.0 ? ratio : 1.0) * 0.95;
    for (int i = 0; i < n_out; i++) {
        double pos = (double)i / ratio;
        int center = (int)pos;
        double frac = pos - center;
        double acc = 0.0, wsum = 0.0;
        for (int j = 0; j < M; j++) {
            int idx = center - (int)half + j;
            if (idx < 0 || idx >= n) continue;
            double t = (double)(j - half + frac);
            double x = M_PI * t * cutoff;
            double sinc = (fabs(t) < 1e-9) ? cutoff : cutoff * sin(x) / x;
            double win = kaiser_bessel_i0(beta * sqrt(1.0 - (t * t) / (half * half)))
                         / i0beta;
            acc += in[idx] * sinc * win;
            wsum += sinc * win;
        }
        out[i] = (wsum > 1e-12) ? (float)(acc / wsum) : 0.0f;
    }
    return n_out;
}
