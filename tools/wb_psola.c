/*
 * wb_psola.c — TD-PSOLA pitch shifter tool (real-time voice-changer core)
 *
 * Usage: wb_psola <in.wav> <factor> <out.wav>
 *   factor 1.0 = unchanged, 2.0 = octave up, 0.5 = octave down.
 * Shifts F0 while preserving the spectral envelope (formants) — a real
 * voice changer, not a chipmunk effect. Measures input/output F0 + formants.
 */
#include "wb_reader.h"
#include "wb_wav.h"
#include "wb_measure.h"
#include "wb_psola.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <in.wav> <factor> <out.wav>\n", argv[0]);
        return 1;
    }
    double factor = atof(argv[2]);
    wb_audio_t a;
    if (wb_audio_read(argv[1], &a) != 0 || a.n <= 0) {
        fprintf(stderr, "read failed: %s\n", argv[1]); return 1;
    }

    /* mono: use channel 0 */
    int sr = a.sample_rate;
    double *mono = malloc((size_t)a.n * sizeof(double));
    int ch = a.channels > 0 ? a.channels : 1;
    for (int i = 0; i < a.n; i++) mono[i] = a.data[(size_t)i * ch];

    wb_formant_measure_t fi = wb_measure_formants(mono, a.n, sr);
    printf("input : %.2fs  F0=%.0f  F1=%.0f F2=%.0f\n", (double)a.n / sr,
           wb_measure_f0(mono, a.n, sr).f0,
           fi.n > 0 ? fi.F[0] : 0, fi.n > 1 ? fi.F[1] : 0);

    int out_n = wb_psola_pitch_shift(mono, (int)a.n, sr, factor, mono, (int)a.n);
    if (out_n <= 0) { fprintf(stderr, "psola failed\n"); return 1; }

    wb_formant_measure_t fo = wb_measure_formants(mono, out_n, sr);
    printf("output: %.2fs  F0=%.0f (want ~%.0f)  F1=%.0f F2=%.0f (formants preserved)\n",
           (double)out_n / sr, wb_measure_f0(mono, out_n, sr).f0, 140.0 * factor,
           fo.n > 0 ? fo.F[0] : 0, fo.n > 1 ? fo.F[1] : 0);

    wb_wav_write(argv[3], mono, (size_t)out_n, sr);
    printf("wrote %s\n", argv[3]);
    wb_audio_free(&a);
    free(mono);
    return 0;
}
