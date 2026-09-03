/* test_ytpmv_machine.c — YTPMV production machine tests (R097) */
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
    
    printf("=== YTPMV Machine (R097) ===\n\n");
    
    /* ---- Stutter Engine ---- */
    printf("--- Stutter Engine ---\n");
    wb_stutter_engine eng;
    wb_stutter_init(&eng, 48000, 2.0f);
    CHECK(eng.buffer != NULL, "stutter: initialized");
    CHECK(eng.buffer_size > 0, "stutter: buffer allocated");
    
    /* Test half-time preset */
    wb_stutter_preset_half_time(&eng);
    CHECK(eng.pattern.n_steps == 16, "stutter: half-time 16 steps");
    CHECK(eng.pattern.values[0] == 1.0f, "stutter: half-time step 0 = 1.0");
    CHECK(eng.pattern.values[8] == 0.0f, "stutter: half-time step 8 = 0.0");
    
    /* Test processing */
    float test_audio[480];
    for (int i = 0; i < 480; i++)
        test_audio[i] = sinf(i * 0.1f);
    
    float stutter_out[480];
    wb_stutter_process_buffer(&eng, stutter_out, test_audio, 480, 1, 120.0f);
    
    /* Check that some samples are muted (stutter effect) */
    int muted = 0, loud = 0;
    for (int i = 0; i < 480; i++) {
        if (fabsf(stutter_out[i]) < 0.01f) muted++;
        if (fabsf(stutter_out[i]) > 0.5f) loud++;
    }
    CHECK(muted > 0, "stutter: some samples muted");
    CHECK(loud > 0, "stutter: some samples loud");
    
    /* Test stutter 16th preset */
    wb_stutter_preset_stutter_16th(&eng);
    CHECK(eng.pattern.values[0] == 1.0f, "stutter: 16th step 0 = 1.0");
    CHECK(eng.pattern.values[1] == 0.0f, "stutter: 16th step 1 = 0.0");
    
    /* Test tape stop */
    wb_stutter_preset_tape_stop(&eng);
    CHECK(eng.pattern.values[0] == 1.0f, "stutter: tape stop starts at 1.0");
    CHECK(eng.pattern.values[31] <= 0.05f, "stutter: tape stop ends at ~0.0");
    
    /* Test gate */
    wb_stutter_preset_gate(&eng, 0.25f);
    CHECK(eng.pattern.values[0] == 1.0f, "stutter: gate starts on");
    CHECK(eng.pattern.values[12] == 0.0f, "stutter: gate 25% duty off after 12");
    
    wb_stutter_free(&eng);
    CHECK(1, "stutter: freed");
    
    /* ---- Formant Shifter ---- */
    printf("\n--- Formant Shifter ---\n");
    wb_formant_shifter fs;
    wb_formant_init(&fs, 2048);
    CHECK(fs.window != NULL, "formant: initialized");
    
    /* Create test signal: 1kHz sine */
    float input[4800], output[4800];
    for (int i = 0; i < 4800; i++)
        input[i] = sinf(i * 2.0f * M_PI * 1000.0f / 48000.0f);
    
    /* Pitch up 2x (octave higher) */
    int out_frames = wb_formant_shift(input, output, 4800, 1, 2.0f, 1.0f);
    CHECK(out_frames == 2400, "formant: 2x pitch = half frames");
    
    /* Pitch down 0.5x (octave lower) */
    out_frames = wb_formant_shift(input, output, 4800, 1, 0.5f, 1.0f);
    CHECK(out_frames == 9600, "formant: 0.5x pitch = double frames");
    
    wb_formant_free(&fs);
    CHECK(1, "formant: freed");
    
    /* ---- Sidechain Compressor ---- */
    printf("\n--- Sidechain Compressor ---\n");
    wb_sidechain_comp comp;
    wb_sidechain_init(&comp, 48000);
    CHECK(comp.gain == 1.0f, "sidechain: starts at unity gain");
    
    wb_sidechain_set_ytpmv(&comp, 0.5f, 4.0f, 5.0f, 150.0f);
    CHECK(comp.threshold == 0.5f, "sidechain: threshold set");
    CHECK(comp.ratio == 4.0f, "sidechain: ratio set");
    
    /* Process with trigger above threshold */
    float trigger = 0.8f; /* Above threshold */
    float signal = 0.5f;
    float out = wb_sidechain_process_ytpmv(&comp, signal, trigger);
    CHECK(out < signal, "sidechain: compression reduces signal");
    
    /* Process multiple frames to let attack engage */
    for (int i = 0; i < 100; i++)
        out = wb_sidechain_process_ytpmv(&comp, signal, trigger);
    CHECK(out < signal * 0.9f, "sidechain: sustained compression");
    
    /* Release: stop triggering */
    for (int i = 0; i < 2000; i++)
        out = wb_sidechain_process_ytpmv(&comp, signal, 0.0f);
    CHECK(out > signal * 0.5f, "sidechain: releases after trigger stops");
    
    /* Internal mode */
    wb_sidechain_comp comp2;
    wb_sidechain_init(&comp2, 48000);
    wb_sidechain_set_ytpmv(&comp2, 0.3f, 8.0f, 3.0f, 100.0f);
    /* Strong signal should trigger compression */
    for (int i = 0; i < 50; i++)
        out = wb_sidechain_process_internal_ytpmv(&comp2, 0.9f);
    CHECK(out < 0.9f, "sidechain: internal mode compresses loud signals");
    
    /* ---- Datamosh ---- */
    printf("\n--- Datamosh ---\n");
    wb_datamosh dm;
    wb_datamosh_init(&dm, 64, 64);
    CHECK(dm.prev_frame != NULL, "datamosh: initialized");
    
    uint8_t frame[64*64*4];
    /* Fill with gradient */
    for (int i = 0; i < 64*64; i++) {
        frame[i*4] = (uint8_t)(i % 256);
        frame[i*4+1] = (uint8_t)((i*2) % 256);
        frame[i*4+2] = (uint8_t)((i*3) % 256);
        frame[i*4+3] = 255;
    }
    
    /* Apply light datamosh */
    wb_datamosh_apply(&dm, frame, 0.3f);
    CHECK(1, "datamosh: applied without crash");
    
    /* Apply heavy datamosh */
    wb_datamosh_apply(&dm, frame, 0.8f);
    CHECK(1, "datamosh: heavy applied without crash");
    
    wb_datamosh_free(&dm);
    CHECK(1, "datamosh: freed");
    
    /* ---- Sentence Mixer ---- */
    printf("\n--- Sentence Mixer ---\n");
    /* Create test audio: 3 segments with different characteristics */
    int sr = 48000;
    float audio[48000]; /* 1 second */
    memset(audio, 0, sizeof(audio));
    
    /* Segment 1: quiet (0-0.3s) */
    for (int i = 0; i < (int)(sr*0.3f); i++)
        audio[i] = 0.01f; /* Very quiet */
    
    /* Segment 2: loud (0.3-0.6s) */
    for (int i = (int)(sr*0.3f); i < (int)(sr*0.6f); i++)
        audio[i] = 0.8f * sinf(i * 0.05f);
    
    /* Segment 3: medium (0.6-1.0s) */
    for (int i = (int)(sr*0.6f); i < sr; i++)
        audio[i] = 0.3f * sinf(i * 0.1f);
    
    wb_phoneme_seg segs[32];
    int n_segs = wb_detect_phonemes_ytpmv(audio, 48000, 1, (float)sr, segs, 32);
    CHECK(n_segs >= 2, "sentence: detected phoneme boundaries");
    
    /* Rearrange: swap segments */
    if (n_segs >= 2) {
        int pattern[2] = {1, 0}; /* Swap first two */
        float output[48000];
        int out_frames = wb_sentence_mix_ytpmv(audio, output, 48000, 1, segs, n_segs, pattern, 2);
        CHECK(out_frames > 0, "sentence: mixed audio");
    }
    
    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    wb_stutter_init(NULL, 48000, 2.0f);
    wb_stutter_free(NULL);
    wb_stutter_process(NULL, 0, 0);
    wb_formant_init(NULL, 0);
    wb_formant_free(NULL);
    wb_sidechain_init(NULL, 0);
    wb_sidechain_process_ytpmv(NULL, 0, 0);
    wb_datamosh_init(NULL, 0, 0);
    wb_datamosh_free(NULL);
    wb_datamosh_apply(NULL, NULL, 0);
    wb_detect_phonemes_ytpmv(NULL, 0, 0, 0, NULL, 0);
    wb_sentence_mix_ytpmv(NULL, NULL, 0, 0, NULL, 0, NULL, 0);
    CHECK(1, "NULL inputs don't crash");
    
    printf("\n=== Results: %d/%d passed ===\n", p, p + f);
    return f > 0 ? 1 : 0;
}
