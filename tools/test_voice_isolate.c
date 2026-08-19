/* test_voice_isolate.c — R018-D spectral voice isolation + FFT sanity.
 *
 * 1. FFT round-trip: real FFT -> inverse == original (within tolerance).
 * 2. Noise suppression: a tone + white noise mixture has lower RMS after
 *    isolation (noise attenuated) while the tone survives (peak preserved).
 * 3. No NaN/Inf in the output. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus/wbus_fft.h"
#include "wbus/wbus_voice_isolate.h"

static int checks = 0, failures = 0;
#define CK(c, m) do { checks++; if (!(c)) { failures++; printf("  [FAIL] %s\n", m); } \
                    else printf("  [PASS] %s\n", m); } while (0)

static double rms(const float *x, int n) {
    double s = 0; for (int i = 0; i < n; i++) s += (double)x[i]*x[i];
    return sqrt(s / n);
}

int main(void) {
    printf("=== R018-D voice isolation + FFT ===\n");

    /* ---- 1. FFT round-trip ---- */
    {
        int N = 1024;
        wb_fft_plan *p = wb_fft_create(N);
        CK(p != NULL, "fft plan created (N=1024)");
        double *x  = (double*)malloc(N*sizeof(double));
        double *re = (double*)malloc(N*sizeof(double));
        double *im = (double*)malloc(N*sizeof(double));
        double *y  = (double*)malloc(N*sizeof(double));
        for (int i = 0; i < N; i++) x[i] = sin(2*M_PI*7*i/N) + 0.3*sin(2*M_PI*53*i/N);
        wb_fft_real(p, x, re, im);
        wb_fft_real_inverse(p, re, im, y);
        double maxerr = 0;
        for (int i = 0; i < N; i++) { double e = fabs(y[i]-x[i]); if (e>maxerr) maxerr=e; }
        CK(maxerr < 1e-9, "FFT round-trip error < 1e-9");
        free(x); free(re); free(im); free(y); wb_fft_destroy(p);
    }

    /* ---- 2. Noise suppression on tone+noise ---- */
    {
        int FR = 44100;                 /* 1 second */
        float *in  = (float*)malloc(FR*sizeof(float));
        float *out = (float*)malloc(FR*sizeof(float));
        unsigned seed = 12345;
        for (int i = 0; i < FR; i++) {
            float tone = 0.4f * (float)sin(2.0*M_PI*220.0*i/44100.0);
            /* deterministic pseudo-noise */
            seed = seed*1103515245u + 12345u;
            float noise = ((float)(seed & 0xffff)/65535.0f - 0.5f) * 0.25f;
            in[i] = tone + noise;
        }
        wb_isolate *iso = wb_isolate_create(0.8f, -40.0f);
        CK(iso != NULL, "isolate context created");
        wb_isolate_process(iso, in, out, FR);

        /* allow a short warmup before measuring (noise floor needs to adapt) */
        int start = 4096;
        double rms_in  = rms(in+start,  FR-start);
        double rms_out = rms(out+start, FR-start);
        CK(rms_out < rms_in, "RMS reduced after isolation (noise attenuated)");
        CK(rms_out > 0.05, "tone survives (RMS still substantial)");

        int nan = 0;
        for (int i = 0; i < FR; i++) if (!isfinite(out[i])) nan++;
        CK(nan == 0, "no NaN/Inf in isolated output");

        /* peak tone preservation: max abs of output should still be near input's tone level */
        float pin = 0, pout = 0;
        for (int i = start; i < FR; i++) { if (fabsf(in[i])>pin) pin=fabsf(in[i]); if (fabsf(out[i])>pout) pout=fabsf(out[i]); }
        CK(pout > 0.2f, "tone peak preserved (pout>0.2)");
        wb_isolate_destroy(iso);
        free(in); free(out);
    }

    /* ---- 3. Stereo path runs without error ---- */
    {
        int FR = 8192;
        float *in  = (float*)malloc(2*FR*sizeof(float));
        float *out = (float*)malloc(2*FR*sizeof(float));
        for (int i = 0; i < 2*FR; i++) in[i] = (float)sin(2.0*M_PI*0.01*i) * 0.2f;
        wb_isolate *iso = wb_isolate_create(0.7f, -40.0f);
        wb_isolate_process_stereo(iso, in, out, FR);
        int nan = 0; for (int i = 0; i < 2*FR; i++) if (!isfinite(out[i])) nan++;
        CK(nan == 0, "stereo isolation: no NaN/Inf");
        wb_isolate_destroy(iso);
        free(in); free(out);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
