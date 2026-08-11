/*
 * wb_master.c — Big Mac MASTERING (stolen-from-wuburvc, pure DSP)
 *
 * Applies the wuburvc mastering chain to any render so Big Mac's voices
 * sound like finished records in GarageBand:
 *   EQ (biquad) -> compressor -> saturation -> stereo width -> limiter
 *   -> loudness normalize (RMS target, true-peak safety)
 *
 * This is the NON-neural part of wuburvc — the neural voice (RMVPE/
 * HuBERT/flow/NSF) stays OUT; Big Mac remains pure articulatory synthesis.
 *
 * Usage: wb_master <in.wav> <out.wav>
 */
#include "wb_reader.h"
#include "wb_wav.h"
#include "wuburvc/wubu_master.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: wb_master <in.wav> <out.wav>\n");
        return 1;
    }
    wb_audio_t a;
    if (wb_audio_read(argv[1], &a) != 0) { fprintf(stderr, "read fail\n"); return 1; }

    /* mono -> interleaved stereo (L=R) for the master chain */
    float *lr = malloc((size_t)a.n * 2 * sizeof(float));
    if (!lr) { wb_audio_free(&a); return 1; }
    for (size_t i = 0; i < a.n; i++) {
        lr[2*i] = (float)a.data[i];
        lr[2*i+1] = (float)a.data[i];
    }

    WuBuMasterOpts opts;
    wubu_master_default(&opts);
    /* gentle broadcast-style touch: HPF rumble cut 40 Hz, light comp,
     * limiter -1 dBTP, RMS -18 */
    opts.eq[0].type = WUBU_EQ_HIGHPASS;
    opts.eq[0].f = 40.0f;
    opts.eq[0].q = 0.707f;
    opts.n_eq = 1;

    int rc = wubu_master_process(&opts, lr, (int)a.n, a.sample_rate);
    if (rc != 0) { fprintf(stderr, "master failed\n"); free(lr); wb_audio_free(&a); return 1; }

    /* stereo -> mono for our writer */
    double *mono = malloc((size_t)a.n * sizeof(double));
    for (size_t i = 0; i < a.n; i++) mono[i] = 0.5 * (lr[2*i] + lr[2*i+1]);

    /* measure the result with our own analyzer */
    float peak = 0, rms = 0;
    for (size_t i = 0; i < a.n; i++) {
        float v = (float)fabs(mono[i]);
        if (v > peak) peak = v;
        rms += mono[i] * mono[i];
    }
    rms = sqrtf(rms / (float)a.n);
    float peak_db = 20.0f * log10f(peak > 1e-9f ? peak : 1e-9f);
    float rms_db = 20.0f * log10f(rms > 1e-9f ? rms : 1e-9f);

    wb_wav_write(argv[2], mono, a.n, a.sample_rate);
    printf("mastered %s -> %s\n", argv[1], argv[2]);
    printf("  peak %.1f dBFS  RMS %.1f dBFS  (target: peak <= -1, RMS ~ -18)\n",
           peak_db, rms_db);

    free(mono); free(lr); wb_audio_free(&a);
    return 0;
}
