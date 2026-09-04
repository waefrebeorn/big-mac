/* test_melody_follow.c — Melody-Following Pitch Mapper tests (R112) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(cond, msg) do { \
    if (cond) { passed++; } \
    else { failed++; fprintf(stderr, "  FAIL: %s\n", msg); } \
} while(0)

int main(void) {
    int passed = 0, failed = 0;
    
    fprintf(stderr, "=== R112: Melody-Following Pitch Mapper ===\n");
    
    /* --- Melody container tests --- */
    fprintf(stderr, "\n-- Melody Container --\n");
    
    wb_melody m;
    wb_melody_init(&m, 120.0f, 4.0f);
    CHECK(m.bpm == 120.0f, "melody: bpm set");
    CHECK(m.n_events == 0, "melody: starts empty");
    
    /* Add some notes */
    wb_melody_add_note(&m, 0.0f, 0.5f, 60, 1.0f); /* C4 */
    wb_melody_add_note(&m, 0.5f, 0.5f, 64, 0.9f); /* E4 */
    wb_melody_add_note(&m, 1.0f, 0.5f, 67, 0.9f); /* G4 */
    wb_melody_add_note(&m, 1.5f, 0.5f, 72, 1.0f); /* C5 */
    CHECK(m.n_events == 4, "melody: 4 notes added");
    
    /* Query notes at specific times */
    CHECK(wb_melody_note_at(&m, 0.0f) == 60, "melody: C4 at t=0");
    CHECK(wb_melody_note_at(&m, 0.25f) == 60, "melody: C4 at t=0.25");
    CHECK(wb_melody_note_at(&m, 0.5f) == 64, "melody: E4 at t=0.5");
    CHECK(wb_melody_note_at(&m, 1.0f) == 67, "melody: G4 at t=1.0");
    CHECK(wb_melody_note_at(&m, 1.5f) == 72, "melody: C5 at t=1.5");
    CHECK(wb_melody_note_at(&m, 2.0f) == 0, "melody: rest at t=2.0");
    CHECK(wb_melody_note_at(&m, 100.0f) == 0, "melody: out of range = rest");
    
    /* Frequency queries */
    float f = wb_melody_freq_at(&m, 0.0f);
    CHECK(f > 260.0f && f < 263.0f, "melody: C4 freq ~261.6Hz");
    f = wb_melody_freq_at(&m, 1.5f);
    CHECK(f > 523.0f && f < 524.0f, "melody: C5 freq ~523.2Hz");
    
    /* --- C major scale builder --- */
    fprintf(stderr, "\n-- C Major Builder --\n");
    
    wb_melody scale;
    wb_melody_build_c_major(&scale, 120.0f, 1);
    CHECK(scale.n_events > 0, "melody: C major has notes");
    CHECK(scale.n_events == 16, "melody: C major 16 notes (up+down)");
    fprintf(stderr, "  C major: %d notes\n", scale.n_events);
    
    /* First note should be C4 (60), last should be C4 (60) — use note_at */
    CHECK(wb_melody_note_at(&scale, 0.0f) == 60, "melody: C major starts on C4");
    CHECK(wb_melody_note_at(&scale, scale.total_duration - 0.01f) == 60, "melody: C major ends on C4");
    
    /* --- From notes array --- */
    fprintf(stderr, "\n-- From Notes Array --\n");
    
    int midi[] = {60, 62, 64, 65, 67, 69, 71, 72};
    float durs[] = {0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.5f};
    wb_melody m2;
    wb_melody_from_notes(&m2, midi, durs, 8, 120.0f);
    CHECK(m2.n_events == 8, "melody: from_notes 8 events");
    CHECK(wb_melody_note_at(&m2, 0.0f) == 60, "melody: first note C4");
    CHECK(wb_melody_note_at(&m2, 1.75f) == 72, "melody: last note C5");
    
    /* --- Mapper tests --- */
    fprintf(stderr, "\n-- Melody Mapper --\n");
    
    wb_melody_mapper mm;
    wb_mapper_init(&mm, 44100.0f);
    CHECK(mm.sample_rate == 44100.0f, "mapper: sample rate set");
    CHECK(mm.n_phonemes == 0, "mapper: starts with 0 phonemes");
    
    /* Create source audio: 5 phonemes at different pitches */
    float starts[] = {0.0f, 0.3f, 0.6f, 0.9f, 1.2f};
    float durs2[] = {0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    int n_ph = 5;
    
    /* Create a target melody: C E G C5 D */
    wb_melody target;
    wb_melody_init(&target, 120.0f, 2.0f);
    wb_melody_add_note(&target, 0.0f, 0.3f, 60, 1.0f);
    wb_melody_add_note(&target, 0.3f, 0.3f, 64, 1.0f);
    wb_melody_add_note(&target, 0.6f, 0.3f, 67, 1.0f);
    wb_melody_add_note(&target, 0.9f, 0.3f, 72, 1.0f);
    wb_melody_add_note(&target, 1.2f, 0.3f, 74, 1.0f);
    
    mm.target = target;
    
    /* Create source audio (sine waves at various frequencies) */
    int total_samples = 44100 * 2; /* 2 seconds */
    mm.source_audio = (float *)calloc(total_samples, sizeof(float));
    mm.source_frames = total_samples;
    mm.source_channels = 1;
    
    /* Fill with different frequency sine waves per phoneme */
    float freqs[] = {220.0f, 330.0f, 440.0f, 294.0f, 392.0f};
    for (int i = 0; i < n_ph; i++) {
        int start_sample = (int)(starts[i] * 44100);
        int dur_sample = (int)(durs2[i] * 44100);
        for (int j = 0; j < dur_sample && (start_sample + j) < total_samples; j++) {
            mm.source_audio[start_sample + j] = sinf(2.0f * M_PI * freqs[i] * j / 44100.0f);
        }
    }
    
    /* Assign phonemes to melody */
    wb_mapper_assign(&mm, starts, durs2, n_ph);
    CHECK(mm.n_phonemes == 5, "mapper: 5 phonemes assigned");
    CHECK(mm.phoneme_target_midi[0] == 60, "mapper: phoneme 0 → C4");
    CHECK(mm.phoneme_target_midi[1] == 64, "mapper: phoneme 1 → E4");
    CHECK(mm.phoneme_target_midi[2] == 67, "mapper: phoneme 2 → G4");
    CHECK(mm.phoneme_target_midi[3] == 72, "mapper: phoneme 3 → C5");
    CHECK(mm.phoneme_target_midi[4] == 74, "mapper: phoneme 4 → D5");
    
    fprintf(stderr, "  Target notes: %d %d %d %d %d\n",
            mm.phoneme_target_midi[0], mm.phoneme_target_midi[1],
            mm.phoneme_target_midi[2], mm.phoneme_target_midi[3],
            mm.phoneme_target_midi[4]);
    
    /* Render */
    float *output = (float *)calloc(total_samples, sizeof(float));
    int rendered = wb_mapper_render(&mm, output, total_samples);
    CHECK(rendered > 0, "mapper: rendered output");
    
    /* Check output has non-zero samples */
    int nonzero = 0;
    for (int i = 0; i < total_samples; i++) {
        if (fabsf(output[i]) > 0.001f) nonzero++;
    }
    CHECK(nonzero > 0, "mapper: output has audio");
    fprintf(stderr, "  Rendered: %d samples, %d nonzero (%.1f%%)\n",
            rendered, nonzero, 100.0f * nonzero / total_samples);
    
    /* Check output has energy in expected time regions */
    int start_region = 0;
    for (int i = 0; i < (int)(0.25f * 44100); i++) {
        if (fabsf(output[i]) > 0.01f) start_region++;
    }
    CHECK(start_region > 100, "mapper: output has energy in first phoneme region");
    
    /* NULL safety */
    wb_mapper_init(NULL, 44100);
    wb_mapper_assign(NULL, starts, durs2, n_ph);
    wb_mapper_assign(&mm, NULL, durs2, n_ph);
    wb_mapper_assign(&mm, starts, NULL, n_ph);
    wb_mapper_render(NULL, output, total_samples);
    wb_mapper_render(&mm, NULL, total_samples);
    wb_mapper_free(NULL);
    CHECK(1, "mapper: NULL safety");
    
    free(output);
    free(mm.source_audio);
    wb_mapper_free(&mm);
    
    /* --- Summary --- */
    fprintf(stderr, "\n=== R112 Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
