/*
 * wb_compare.c — TTS comparison harness (gaps H91-96)
 *
 * Scores one TTS output against a reference on the axes that matter for
 * intelligibility/naturalness grounding:
 *   - duration match (%)
 *   - F0 match (mean + range)
 *   - voiced fraction match
 *   - formant centroid match
 *   - overall RMS balance
 *
 * Usage: wb_compare <ours.wav> <reference.wav>
 */
#include "wb_reader.h"
#include "wb_measure.h"
#include "wb_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: wb_compare <ours.wav> <reference.wav>\n");
        return 1;
    }
    wb_audio_t a, r;
    if (wb_audio_read(argv[1], &a) != 0) { fprintf(stderr, "read fail %s\n", argv[1]); return 1; }
    if (wb_audio_read(argv[2], &r) != 0) { fprintf(stderr, "read fail %s\n", argv[2]); return 1; }

    printf("=== TTS COMPARISON ===\n");
    printf("%-24s %10s %10s   %s\n", "metric", argv[1], "reference", "match");

    /* duration */
    double dur_a = (double)a.n / a.sample_rate;
    double dur_r = (double)r.n / r.sample_rate;
    double dur_match = dur_r > 0 ? (1.0 - fabs(dur_a - dur_r) / dur_r) * 100 : 0;
    printf("%-24s %8.2fs %8.2fs   %5.1f%%\n", "duration", dur_a, dur_r, dur_match < 0 ? 0 : dur_match);

    /* F0 */
    wb_f0_measure_t fa = wb_measure_f0(a.data, a.n, a.sample_rate);
    wb_f0_measure_t fr = wb_measure_f0(r.data, r.n, r.sample_rate);
    double f0_match = (fr.f0_mean > 0 && fa.f0_mean > 0)
        ? (1.0 - fabs(fa.f0_mean - fr.f0_mean) / fr.f0_mean) * 100 : 0;
    printf("%-24s %8.1fHz %8.1fHz   %5.1f%%\n", "f0 mean", fa.f0_mean, fr.f0_mean, f0_match < 0 ? 0 : f0_match);

    /* voicing */
    double v_match = fr.voiced_fraction > 0
        ? (1.0 - fabs(fa.voiced_fraction - fr.voiced_fraction) / fr.voiced_fraction) * 100 : 0;
    printf("%-24s %8.0f%% %8.0f%%   %5.1f%%\n", "voiced", fa.voiced_fraction * 100,
           fr.voiced_fraction * 100, v_match < 0 ? 0 : v_match);

    /* formants */
    wb_formant_measure_t ma = wb_measure_formants(a.data, a.n, a.sample_rate);
    wb_formant_measure_t mr = wb_measure_formants(r.data, r.n, r.sample_rate);
    double f1a = ma.n > 0 ? ma.F[0] : 0, f1r = mr.n > 0 ? mr.F[0] : 0;
    double f1_match = f1r > 0 ? (1.0 - fabs(f1a - f1r) / f1r) * 100 : 0;
    printf("%-24s %8.0fHz %8.0fHz   %5.1f%%\n", "F1", f1a, f1r, f1_match < 0 ? 0 : f1_match);

    /* RMS balance */
    double rms_a = 0, rms_r = 0;
    for (size_t i = 0; i < a.n; i++) rms_a += a.data[i] * a.data[i];
    for (size_t i = 0; i < r.n; i++) rms_r += r.data[i] * r.data[i];
    rms_a = sqrt(rms_a / a.n); rms_r = sqrt(rms_r / r.n);
    double rms_match = rms_r > 0 ? (1.0 - fabs(rms_a - rms_r) / rms_r) * 100 : 0;
    printf("%-24s %8.3f %8.3f   %5.1f%%\n", "rms", rms_a, rms_r, rms_match < 0 ? 0 : rms_match);

    double avg = (dur_match + f0_match + v_match + f1_match + rms_match) / 5.0;
    if (avg < 0) avg = 0;
    printf("\nOVERALL MATCH: %.1f%%\n", avg);

    wb_audio_free(&a); wb_audio_free(&r);
    return 0;
}
