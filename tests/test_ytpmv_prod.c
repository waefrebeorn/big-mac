/* test_ytpmv_prod.c — YTPMV Production Pipeline tests (R104) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(c, m) do { if (c) { p++; printf("  PASS: %s\n", m); } \
                         else { f++; printf("  FAIL: %s\n", m); } } while(0)

int main(void) {
    int p = 0, f = 0;

    printf("=== R104: YTPMV Production Pipeline ===\n\n");

    /* ---- Pitch Detection ---- */
    printf("--- Pitch Detection ---\n");
    /* Create 440Hz sine wave */
    int sr = 48000;
    int n = 4800;
    float *audio = (float *)malloc(n * sizeof(float));
    for (int i = 0; i < n; i++)
        audio[i] = sinf(i * 2.0f * M_PI * 440.0f / sr);

    float pitch = wb_detect_pitch(audio, n, sr, 80, 800);
    printf("  Detected pitch: %.1f Hz (expected ~440)\n", pitch);
    CHECK(pitch > 400 && pitch < 480, "pitch: detected ~440Hz");

    /* ---- Pitch Correction ---- */
    printf("\n--- Pitch Correction ---\n");
    wb_pitch_correction pc = wb_correct_pitch(440.0f, 0);
    printf("  440Hz -> MIDI %d, target %.1f Hz, ratio %.3f\n", pc.midi_note, pc.target_pitch, pc.ratio);
    CHECK(pc.midi_note == 69, "correction: A4 -> MIDI 69");
    CHECK(fabsf(pc.target_pitch - 440.0f) < 1.0f, "correction: target ~440Hz");

    /* Test with detuned note */
    wb_pitch_correction pc2 = wb_correct_pitch(430.0f, 0);
    printf("  430Hz -> MIDI %d, target %.1f Hz, cents %.0f\n", pc2.midi_note, pc2.target_pitch, pc2.cents_off);
    CHECK(pc2.midi_note == 69, "correction: 430Hz snaps to MIDI 69");
    CHECK(pc2.ratio > 1.0f, "correction: ratio > 1 (pitch up)");

    /* ---- YTPMV Producer ---- */
    printf("\n--- YTPMV Producer ---\n");
    ytpmv_producer prod;
    ytpmv_prod_init(&prod, (float)sr);

    /* Create test audio: 3 segments with different frequencies */
    int seg_len = 16000;
    int total = seg_len * 3;
    float *test_audio = (float *)malloc(total * sizeof(float));
    for (int i = 0; i < seg_len; i++)
        test_audio[i] = sinf(i * 2.0f * M_PI * 261.63f / sr); /* C4 */
    for (int i = 0; i < seg_len; i++)
        test_audio[seg_len + i] = sinf(i * 2.0f * M_PI * 329.63f / sr); /* E4 */
    for (int i = 0; i < seg_len; i++)
        test_audio[2*seg_len + i] = sinf(i * 2.0f * M_PI * 392.00f / sr); /* G4 */

    int n_ph = ytpmv_prod_analyze(&prod, test_audio, total, 1);
    printf("  Detected %d phonemes\n", n_ph);
    CHECK(n_ph >= 1, "producer: detected phonemes");

    /* Check that pitches were detected */
    if (n_ph > 0) {
        printf("  Phoneme 0: pitch=%.1f Hz, MIDI=%d\n", prod.pitches[0], prod.midi_notes[0]);
        CHECK(prod.pitches[0] > 0, "producer: detected pitch for phoneme 0");
    }

    /* Render output */
    int out_frames = total;
    float *output = (float *)calloc(out_frames, sizeof(float));
    int rendered = ytpmv_prod_render(&prod, output, out_frames, 1);
    printf("  Rendered %d frames\n", rendered);
    CHECK(rendered > 0, "producer: rendered audio");

    /* Check output has content */
    float max_out = 0;
    for (int i = 0; i < rendered; i++)
        if (fabsf(output[i]) > max_out) max_out = fabsf(output[i]);
    CHECK(max_out > 0.01f, "producer: output has signal");

    free(audio);
    free(test_audio);
    free(output);

    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    ytpmv_prod_init(NULL, 0);
    ytpmv_prod_analyze(NULL, NULL, 0, 0);
    ytpmv_prod_render(NULL, NULL, 0, 0);
    wb_detect_pitch(NULL, 0, 0, 0, 0);
    wb_correct_pitch(0, 0);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", p, p + f);
    return f > 0 ? 1 : 0;
}
