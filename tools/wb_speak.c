/*
 * wb_speak.c — Big Mac voice tool: render a vowel sequence through the
 * C11 vocal tract to a WAV file.
 *
 * Usage: wb_speak <out.wav> <seconds> [f0]
 */
#include "wb_tract.h"
#include "wb_glottis.h"
#include "wb_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define SAMPLE_RATE 44100
#define BLOCK 1024

int main(int argc, char **argv) {
    const char *out_path = "out.wav";
    double seconds = 1.0;
    double f0 = 140.0;
    if (argc > 1) out_path = argv[1];
    if (argc > 2) seconds = atof(argv[2]);
    if (argc > 3) f0 = atof(argv[3]);
    if (seconds <= 0) seconds = 0.5;
    if (seconds > 30) seconds = 30;

    wb_tract_t *tract = wb_tract_new(44);
    wb_glottis_t *glottis = wb_glottis_new();
    if (!tract || !glottis) { fprintf(stderr, "alloc failed\n"); return 1; }

    wb_glottis_set_frequency(glottis, f0);
    wb_glottis_set_intensity(glottis, 0.8);

    int n_samples = (int)(seconds * SAMPLE_RATE);
    double *out = malloc((size_t)n_samples * sizeof(double));
    if (!out) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* A simple vowel sweep: aa -> iy -> uw -> aa (tongue moves) */
    const double sweep[4][2] = {
        {20.0, 1.1},  /* aa */
        {13.5, 0.6},  /* iy */
        {18.0, 1.4},  /* uw */
        {20.0, 1.1},  /* aa */
    };
    const int sweep_len = 4;

    for (int j = 0; j < n_samples; j++) {
        int m = j % BLOCK;
        double lam1 = (double)m / BLOCK;
        double lam2 = (m + 0.5) / BLOCK;

        /* move tongue over the sweep */
        double t_frac = (double)j / n_samples * (sweep_len - 1);
        int seg = (int)t_frac; if (seg > 3) seg = 3;
        double frac = t_frac - seg;
        double ti = sweep[seg][0] * (1 - frac) + sweep[seg + 1][0] * frac;
        double td = sweep[seg][1] * (1 - frac) + sweep[seg + 1][1] * frac;
        wb_tract_set_rest_diameter(tract, ti, td);
        wb_tract_set_lips(tract, 0.8);

        /* aspiration noise: simple deterministic */
        double noise = (double)((j * 2654435761u) >> 24) / 128.0 - 1.0;
        double gl = wb_glottis_run_step(glottis, lam1, noise * 0.3);
        double vocal = 0.0;
        vocal += wb_tract_run_step(tract, gl, noise * 0.3, lam1);
        vocal += wb_tract_run_step(tract, gl, noise * 0.3, lam2);
        out[j] = vocal * 0.125;

        if (m == BLOCK - 1) {
            wb_glottis_finish_block(glottis, 1, (double)BLOCK / SAMPLE_RATE);
            wb_tract_finish_block(tract, (double)BLOCK / SAMPLE_RATE);
        }
    }

    int rc = wb_wav_write(out_path, out, (size_t)n_samples, SAMPLE_RATE);
    if (rc == 0) {
        /* report real numbers */
        double peak = 0, rms_acc = 0;
        for (int i = 0; i < n_samples; i++) {
            double a = fabs(out[i]);
            if (a > peak) peak = a;
            rms_acc += out[i] * out[i];
        }
        printf("wrote %s: %d samples (%.2fs) peak=%.3f rms=%.4f\n",
               out_path, n_samples, seconds, peak, sqrt(rms_acc / n_samples));
    } else {
        fprintf(stderr, "write failed\n");
    }

    free(out);
    wb_glottis_free(glottis);
    wb_tract_free(tract);
    return rc == 0 ? 0 : 1;
}
