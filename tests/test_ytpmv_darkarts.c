/* test_ytpmv_darkarts.c — YTPMV pipeline + dark arts tests (R094b/c) */
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
    uint8_t *img, *out;
    int w = 64, h = 64, sz = w * h * 4;

    printf("=== YTPMV + Dark Arts (R094b/c) ===\n\n");

    /* ---- Phoneme Database ---- */
    printf("--- Phoneme Database ---\n");
    wb_phoneme_db *db = wb_phoneme_db_create(64);
    CHECK(db != NULL, "phoneme db created");
    int i0 = wb_phoneme_add(db, 0.0f, 0.5f, 440.0f, 0.8f, PHON_VOWEL_A);
    CHECK(i0 == 0, "added phoneme 0");
    int i1 = wb_phoneme_add(db, 0.5f, 1.0f, 220.0f, 0.6f, PHON_CONSONANT_B);
    CHECK(i1 == 1, "added phoneme 1");
    int i2 = wb_phoneme_add(db, 1.0f, 1.5f, 880.0f, 0.9f, PHON_VOWEL_I);
    CHECK(i2 == 2, "added phoneme 2");
    CHECK(db->count == 3, "db count = 3");
    CHECK(db->total_duration == 1.5f, "db duration = 1.5");

    /* ---- Pitch-to-Note Mapping ---- */
    printf("\n--- Pitch-to-Note Mapping ---\n");
    int a4 = freq_to_midi(440.0f);
    CHECK(a4 == 69, "freq 440Hz = MIDI 69 (A4)");
    int c4 = freq_to_midi(261.63f);
    CHECK(c4 == 60, "freq 261.63Hz = MIDI 60 (C4)");
    float freq = midi_to_freq(69);
    CHECK(fabsf(freq - 440.0f) < 1.0f, "MIDI 69 = ~440Hz");
    CHECK(strcmp(midi_note_name(60), "C") == 0, "MIDI 60 = C");
    CHECK(midi_note_octave(60) == 4, "MIDI 60 = octave 4");

    /* ---- Scale Quantization ---- */
    printf("\n--- Scale Quantization ---\n");
    int q = midi_quantize_to_scale(61, SCALE_MAJOR, 60); /* C# in C major → C or D */
    CHECK(q == 60 || q == 62, "C# quantized to C major scale");
    int in_scale = midi_in_scale(64, SCALE_MAJOR, 60); /* E is in C major */
    CHECK(in_scale == 1, "E is in C major");
    int not_in = midi_in_scale(61, SCALE_MAJOR, 60); /* C# NOT in C major */
    CHECK(not_in == 0, "C# is NOT in C major");

    /* Pentatonic */
    int pent = midi_quantize_to_scale(63, SCALE_PENTATONIC, 60);
    CHECK(midi_in_scale(pent, SCALE_PENTATONIC, 60), "pentatonic quantize result in scale");

    /* ---- Beat Sequencer ---- */
    printf("\n--- Beat Sequencer ---\n");
    wb_sequencer seq;
    wb_sequencer_init(&seq, 120.0f, 16);
    wb_sequencer_set_note(&seq, 0, 0, 60, 100);
    wb_sequencer_set_note(&seq, 0, 4, 64, 90);
    wb_sequencer_set_note(&seq, 0, 8, 67, 110);
    wb_sequencer_set_note(&seq, 0, 12, 72, 127);
    wb_sequencer_start(&seq);
    CHECK(wb_sequencer_current_note(&seq, 0) == 60, "seq step 0 = note 60");
    wb_sequencer_tick(&seq, 0);
    wb_sequencer_tick(&seq, 0);
    wb_sequencer_tick(&seq, 0);
    CHECK(wb_sequencer_current_note(&seq, 0) == -1, "seq step 3 = silent");
    wb_sequencer_tick(&seq, 0);
    CHECK(wb_sequencer_current_note(&seq, 0) == 64, "seq step 4 = note 64");

    /* ---- YTPMV Renderer ---- */
    printf("\n--- YTPMV Renderer ---\n");
    wb_ytpmv_renderer *r = wb_ytpmv_create(db, 140.0f);
    CHECK(r != NULL, "renderer created");
    r->scale = SCALE_MINOR;
    r->root_note = 60;
    CHECK(r->master_bpm == 140.0f, "renderer BPM = 140");
    wb_ytpmv_free(r);
    CHECK(1, "renderer freed");

    /* ---- Compression Torture ---- */
    printf("\n--- Compression Torture ---\n");
    img = (uint8_t *)calloc(sz, 1);
    for (int i = 0; i < w*h; i++) {
        img[i*4+0] = (uint8_t)(i % 256);
        img[i*4+1] = (uint8_t)((i*2) % 256);
        img[i*4+2] = (uint8_t)((i*3) % 256);
        img[i*4+3] = 255;
    }
    wb_compression_torture(img, w, h, 10);
    CHECK(img[0*4+3] == 255, "torture: alpha preserved");
    /* After heavy quantization, colors should be reduced */
    int distinct = 0;
    uint8_t seen[256] = {0};
    for (int i = 0; i < w*h; i++) {
        if (!seen[img[i*4]]) { seen[img[i*4]] = 1; distinct++; }
    }
    CHECK(distinct < 256, "torture: color levels reduced");

    /* ---- Stare Down / Mysterious Zoom ---- */
    printf("\n--- Stare Down / Mysterious Zoom ---\n");
    out = (uint8_t *)calloc(sz, 1);
    for (int i = 0; i < w*h; i++) { img[i*4] = 200; img[i*4+3] = 255; }
    wb_stare_down(out, img, w, h, 0.5f, 0.5f, 0.5f);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "stare_down: center visible");

    wb_mysterious_zoom(out, img, w, h, 0.3f, 5.0f, 0.5f, 0.5f);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "mysterious_zoom: center visible");

    /* ---- Bleep Censor ---- */
    printf("\n--- Bleep Censor ---\n");
    memset(img, 128, sz);
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    wb_bleep_bar(img, w, h, 20, 20, 44, 44, 0, 0, 0);
    int bar_idx = (30 * w + 30) * 4;
    CHECK(img[bar_idx] == 0 && img[bar_idx+1] == 0 && img[bar_idx+2] == 0, "bleep: bar is black");
    CHECK(img[0] == 128, "bleep: outside bar unchanged");

    /* ---- MLG Flash ---- */
    printf("\n--- MLG Flash ---\n");
    memset(img, 128, sz);
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    wb_mlg_flash(img, w, h, 0.5f);
    CHECK(img[0*4] > 128, "mlg: red channel boosted");
    CHECK(img[0*4+3] == 255, "mlg: alpha preserved");

    /* ---- Saponite ---- */
    printf("\n--- Saponite ---\n");
    for (int i = 0; i < w*h; i++) {
        img[i*4] = 200; img[i*4+1] = 100; img[i*4+2] = 50; img[i*4+3] = 255;
    }
    wb_saponite(out, img, w, h, 0.2f, 0.5f);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "saponite: output has content");

    /* ---- Infinite Loop Blend ---- */
    printf("\n--- Infinite Loop Blend ---\n");
    uint8_t *frames = (uint8_t *)calloc(sz * 8, 1);
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < w*h; j++) {
            frames[i*sz + j*4] = (uint8_t)(i * 30);
            frames[i*sz + j*4+3] = 255;
        }
    }
    wb_infinite_loop_blend(frames, w, h, 8, 2);
    CHECK(frames[0*sz+0*4+3] == 255, "loop_blend: alpha preserved");

    /* ---- Mad Dash Cuts ---- */
    printf("\n--- Mad Dash Cuts ---\n");
    int cuts[32];
    int n_cuts = wb_mad_dash_cuts(1000, 8, cuts);
    CHECK(n_cuts > 0, "mad_dash: cuts generated");
    CHECK(n_cuts <= 8, "mad_dash: cuts <= requested");
    /* Cuts should be accelerating (gaps decreasing) */
    if (n_cuts >= 4) {
        int gap1 = cuts[n_cuts/2] - cuts[n_cuts/2-1];
        int gap2 = cuts[n_cuts-1] - cuts[n_cuts-2];
        CHECK(gap2 < gap1, "mad_dash: gaps decrease (accelerating)");
    }

    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    wb_compression_torture(NULL, w, h, 10);
    wb_stare_down(NULL, NULL, w, h, 0, 0, 0);
    wb_mysterious_zoom(NULL, NULL, w, h, 0, 0, 0, 0);
    wb_bleep_bar(NULL, w, h, 0, 0, 0, 0, 0, 0, 0);
    wb_mlg_flash(NULL, w, h, 0);
    wb_saponite(NULL, NULL, w, h, 0, 0);
    wb_infinite_loop_blend(NULL, w, h, 0, 0);
    wb_mad_dash_cuts(0, 0, NULL);
    wb_phoneme_db_free(NULL);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", p, p + f);

    free(img);
    free(out);
    free(frames);
    wb_phoneme_db_free(db);

    return f > 0 ? 1 : 0;
}
