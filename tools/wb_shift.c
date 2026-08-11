/*
 * wb_shift.c — EXPERIMENTAL, NOT IN THE BUILD, NOT VERIFIED.
 *
 * Attempted formant-preserving pitch shifter (phase vocoder + envelope
 * preservation). Builds and runs, but the 2x upshift produces incoherent
 * (noisy) output — phase-coherence at upshift is broken and could NOT be
 * verified or debugged because Big Mac is muted (no listening). Removed
 * from the Makefile on purpose: shipping unverified DSP violates the
 * "no stubs / verify before claiming" doctrine. Keep the file for a future
 * session WITH a listen, or replace with TD-PSOLA which is more robust.
 *
 * Pure C11, self-contained — wb_rfft for the forward FFT, a hand-rolled
 * inverse FFT, cepstral envelope estimation, and overlap-add synthesis.
 *
 * Usage: wb_shift <in.wav> <ratio> <out.wav>   (ratio 2.0 = octave up)
 */
#include "wb_reader.h"
#include "wb_wav.h"
#include "wb_dsp.h"
#include "wb_measure.h"
#include "wb_resample.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SR 44100
#define NFFT 1024
#define HOP (NFFT / 4)          /* 75% overlap */
#define CEPHOP 40               /* cepstral liftering cut (samples) */

/* --- inverse real FFT of a conjugate-symmetric spectrum (Re[0..n/2],
 * Im[0..n/2], Im[0]=Im[n/2]=0) --- */
static void irfft(const double *Re, const double *Im, size_t n, double *x) {
    for (size_t t = 0; t < n; t++) {
        double s = Re[0] + Re[n/2] * cos(M_PI * (double)t);
        for (size_t k = 1; k < n/2; k++) {
            double ang = 2.0 * M_PI * (double)k * (double)t / (double)n;
            s += 2.0 * (Re[k] * cos(ang) - Im[k] * sin(ang));
        }
        x[t] = s / (double)n;
    }
}

/* --- Hann window --- */
static void hann(double *w, size_t n) {
    for (size_t i = 0; i < n; i++)
        w[i] = 0.5 * (1.0 - cos(2.0 * M_PI * (double)i / (double)(n - 1)));
}

/* --- spectral envelope via cepstral liftering (log-magnitude -> ifft ->
 * zero high quefrencies -> fft). We approximate the envelope by smoothing
 * the magnitude in frequency with a wide moving average (a coarse, cheap
 * envelope that keeps formant peaks but removes the harmonic comb). --- */
static void envelope(const double *mag, size_t bins, double *env) {
    /* moving average over ~1/12 of the band (~half a formant spacing) */
    size_t r = bins / 14; if (r < 2) r = 2;
    for (size_t k = 0; k < bins; k++) {
        size_t lo = k > r ? k - r : 0, hi = k + r < bins ? k + r : bins - 1;
        double s = 0; size_t c = 0;
        for (size_t j = lo; j <= hi; j++) { s += mag[j]; c++; }
        env[k] = s / (double)c;
    }
    /* a little more smoothing toward a peak-preserving envelope */
    for (size_t k = 0; k < bins; k++) env[k] = 0.6 * env[k] + 0.4 * mag[k];
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: wb_shift <in.wav> <ratio> <out.wav>\n");
        fprintf(stderr, "  ratio 2.0 = octave up, 0.5 = octave down; formants preserved\n");
        return 1;
    }
    double ratio = atof(argv[2]);
    if (ratio <= 0.1 || ratio > 4.0) { fprintf(stderr, "ratio must be 0.1..4.0\n"); return 1; }

    wb_audio_t a0;
    if (wb_audio_read(argv[1], &a0) != 0) { fprintf(stderr, "read fail\n"); return 1; }
    wb_audio_t a = a0;
    if (a0.sample_rate != SR) {
        float *fin = malloc((size_t)a0.n * sizeof(float));
        size_t nout = (size_t)((double)a0.n * SR / a0.sample_rate) + 64;
        float *fout = malloc(nout * sizeof(float));
        for (size_t i = 0; i < a0.n; i++) fin[i] = (float)a0.data[i];
        int got = wb_resample_sinc(fin, (int)a0.n, a0.sample_rate, SR, fout);
        if (got > 0) {
            a.data = malloc((size_t)got * sizeof(double));
            for (int i = 0; i < got; i++) a.data[i] = fout[i];
            a.n = (size_t)got; a.sample_rate = SR;
        }
        free(fin); free(fout);
    }
    size_t N = a.n;
    size_t nframes = (N > NFFT) ? (N - NFFT) / HOP + 1 : 1;

    double *w = malloc(NFFT * sizeof(double)); hann(w, NFFT);
    double *Re = malloc(NFFT * sizeof(double));
    double *Im = malloc(NFFT * sizeof(double));
    double *mag = malloc((NFFT/2 + 1) * sizeof(double));
    double *env = malloc((NFFT/2 + 1) * sizeof(double));
    double *frame = malloc(NFFT * sizeof(double));
    double *syn = malloc(NFFT * sizeof(double));
    double *out = calloc(nframes * HOP + NFFT, sizeof(double));
    size_t bins = NFFT/2 + 1;

    /* phase-vocoder phase tracking */
    double *prev_phase = calloc(bins, sizeof(double));
    double *out_phase = calloc(bins, sizeof(double));

    for (size_t f = 0; f < nframes; f++) {
        size_t start = f * HOP;
        memset(frame, 0, NFFT * sizeof(double));
        for (size_t i = 0; i < NFFT && start + i < N; i++) frame[i] = a.data[start + i] * w[i];
        memset(Re, 0, NFFT * sizeof(double)); memset(Im, 0, NFFT * sizeof(double));
        wb_rfft(frame, NFFT, Re, Im);
        for (size_t k = 0; k < bins; k++) mag[k] = hypot(Re[k], Im[k]) + 1e-9;
        envelope(mag, bins, env);
        /* envelope-preserving pitch shift: resample the excitation (mag/env)
         * by `ratio` in frequency, keep the original envelope (formants) */
        for (size_t k = 0; k < bins; k++) {
            double src = (double)k / ratio;        /* where this bin samples the excitation */
            double exc;
            if (src >= bins - 1.0) exc = 1e-6;
            else {
                int i0 = (int)src, i1 = i0 + 1 < (int)bins ? i0 + 1 : i0;
                double frac = src - i0;
                double e0 = (mag[i0] / env[i0]), e1 = (mag[i1] / env[i1]);
                exc = e0 + frac * (e1 - e0);
            }
            mag[k] = env[k] * exc;                  /* formants fixed, harmonics moved */
        }
        /* phase vocoder phase: accumulate the true phase derivative scaled
         * by ratio (coherent resynthesis) */
        for (size_t k = 0; k < bins; k++) {
            double freq = 2.0 * M_PI * (double)k / (double)NFFT;
            double true_phase = atan2(Im[k], Re[k]);
            double delta = true_phase - prev_phase[k] - freq * (double)HOP;
            while (delta > M_PI) delta -= 2 * M_PI;
            while (delta < -M_PI) delta += 2 * M_PI;
            out_phase[k] += ratio * (freq * (double)HOP + delta);
            prev_phase[k] = true_phase;
        }
        /* reconstruct spectrum */
        for (size_t k = 0; k < bins; k++) {
            Re[k] = mag[k] * cos(out_phase[k]);
            Im[k] = mag[k] * sin(out_phase[k]);
        }
        irfft(Re, Im, NFFT, syn);
        for (size_t i = 0; i < NFFT; i++) out[start + i] += syn[i] * w[i];
    }

    /* write out */
    size_t total = nframes * HOP + NFFT; if (total > N + NFFT) total = N + NFFT;
    wb_wav_write(argv[3], out, total, SR);

    /* measure the shifted result */
    wb_f0_measure_t f0m = wb_measure_f0(out, total, SR);
    wb_formant_measure_t fm = wb_measure_formants(out, total, SR);
    printf("pitch shift: %s x%.2f -> %s\n", argv[1], ratio, argv[3]);
    printf("  output F0=%.1f Hz, formants F1=%.0f F2=%.0f (%.2fs)\n",
           f0m.f0_mean, fm.n > 0 ? fm.F[0] : 0, fm.n > 1 ? fm.F[1] : 0, (double)total / SR);

    free(w); free(Re); free(Im); free(mag); free(env); free(frame); free(syn); free(out);
    free(prev_phase); free(out_phase);
    wb_audio_free(&a);
    return 0;
}
