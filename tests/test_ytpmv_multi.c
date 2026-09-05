/* test_ytpmv_multi.c — Tests for Multi-Character YTPMV Engine (R131) */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "wbus/wbus_compositor.h"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); passes++; } \
} while(0)

int main() {
    int passes = 0, failures = 0;
    
    /* Test 1: Character init */
    ytpmv_character ch;
    ytpmv_character_init(&ch, "SpongeBob");
    ASSERT(strcmp(ch.name, "SpongeBob") == 0, "Character name set");
    ASSERT(ch.n_samples == 0, "Character starts with 0 samples");
    
    /* Test 2: Add samples */
    int s0 = ytpmv_character_add_sample(&ch, "vowel_a", 1.0f, 0.15f, 220.0f);
    ASSERT(s0 == 0, "First sample added at index 0");
    ASSERT(ch.n_samples == 1, "Sample count = 1");
    ASSERT(ch.samples[0].midi_note == 57, "220Hz = MIDI 57 (A3)");
    
    int s1 = ytpmv_character_add_sample(&ch, "vowel_e", 2.0f, 0.20f, 330.0f);
    ASSERT(s1 == 1, "Second sample added at index 1");
    ASSERT(ch.samples[1].midi_note == 64, "330Hz = MIDI 64 (E4)");
    
    int s2 = ytpmv_character_add_sample(&ch, "vowel_o", 3.0f, 0.10f, 440.0f);
    ASSERT(s2 == 2, "Third sample added at index 2");
    ASSERT(ch.samples[2].midi_note == 69, "440Hz = MIDI 69 (A4)");
    
    /* Test 3: Pitch range tracking */
    ASSERT(ch.min_midi == 57, "Min MIDI = 57");
    ASSERT(ch.max_midi == 69, "Max MIDI = 69");
    
    /* Test 4: Sample selection */
    float ratio = 1.0f;
    int best = ytpmv_character_select_sample(&ch, 60, &ratio); /* Target C4 */
    ASSERT(best == 0, "MIDI 60 closest to sample 0 (MIDI 57, dist=3)");
    ASSERT(ratio > 1.0f, "Pitch ratio > 1 (need to shift up)");
    
    best = ytpmv_character_select_sample(&ch, 68, &ratio); /* Target G#4 */
    ASSERT(best == 2, "MIDI 68 closest to sample 2 (MIDI 69, dist=1)");
    
    /* Test 5: Harmony - single */
    ytpmv_harmony h;
    ytpmv_harmony_init(&h);
    ASSERT(h.n_voices == 1, "Default harmony = 1 voice");
    ASSERT(h.intervals[0] == 0, "Single voice at interval 0");
    
    /* Test 6: Harmony - major chord */
    ytpmv_harmony_major(&h);
    ASSERT(h.n_voices == 3, "Major chord = 3 voices");
    ASSERT(h.intervals[0] == 0, "Root at 0");
    ASSERT(h.intervals[1] == 4, "Major 3rd at 4");
    ASSERT(h.intervals[2] == 7, "Perfect 5th at 7");
    
    /* Test 7: Harmony - minor chord */
    ytpmv_harmony_minor(&h);
    ASSERT(h.n_voices == 3, "Minor chord = 3 voices");
    ASSERT(h.intervals[1] == 3, "Minor 3rd at 3");
    
    /* Test 8: Harmony - power chord */
    ytpmv_harmony_power(&h);
    ASSERT(h.n_voices == 3, "Power chord = 3 voices");
    ASSERT(h.intervals[1] == 7, "5th at 7");
    ASSERT(h.intervals[2] == 12, "Octave at 12");
    
    /* Test 9: Harmony - duet */
    ytpmv_harmony_duet(&h);
    ASSERT(h.n_voices == 2, "Duet = 2 voices");
    ASSERT(h.intervals[1] == 4, "Duet at +4 semitones");
    
    /* Test 10: Harmony generation */
    ytpmv_harmony_major(&h);
    int notes[4]; float vols[4];
    int n = ytpmv_generate_harmony(60, &h, notes, vols, 4);
    ASSERT(n == 3, "Generated 3 harmony notes");
    ASSERT(notes[0] == 60, "Root = 60");
    ASSERT(notes[1] == 64, "3rd = 64");
    ASSERT(notes[2] == 67, "5th = 67");
    ASSERT(vols[0] == 1.0f, "Root volume = 1.0");
    
    /* Test 11: Project init */
    ytpmv_project proj;
    ytpmv_project_init(&proj);
    ASSERT(proj.n_characters == 0, "Project starts empty");
    ASSERT(proj.master_volume == 1.0f, "Master volume = 1.0");
    
    /* Test 12: Add characters */
    int c0 = ytpmv_project_add_character(&proj, "SpongeBob");
    ASSERT(c0 == 0, "First character at index 0");
    int c1 = ytpmv_project_add_character(&proj, "Patrick");
    ASSERT(c1 == 1, "Second character at index 1");
    ASSERT(proj.n_characters == 2, "Project has 2 characters");
    
    /* Test 13: Get character */
    ytpmv_character *ch_ptr = ytpmv_project_get_character(&proj, 0);
    ASSERT(ch_ptr != NULL, "Got character 0");
    ASSERT(strcmp(ch_ptr->name, "SpongeBob") == 0, "Character 0 is SpongeBob");
    
    ch_ptr = ytpmv_project_get_character(&proj, 1);
    ASSERT(ch_ptr != NULL, "Got character 1");
    ASSERT(strcmp(ch_ptr->name, "Patrick") == 0, "Character 1 is Patrick");
    
    /* Test 14: Channel assignment */
    ytpmv_project_assign_channel(&proj, 0, 0);
    ytpmv_project_assign_channel(&proj, 1, 1);
    ASSERT(proj.channel_to_char[0] == 0, "Channel 0 → SpongeBob");
    ASSERT(proj.channel_to_char[1] == 1, "Channel 1 → Patrick");
    
    /* Test 15: Voice pool */
    ytpmv_voice_pool pool;
    ytpmv_voice_pool_init(&pool);
    ASSERT(pool.n_active == 0, "Voice pool starts empty");
    
    int v0 = ytpmv_voice_allocate(&pool, 0, 0, 60, 0.0f, 0.5f, 1.0f, 1.1f);
    ASSERT(v0 >= 0, "Allocated voice 0");
    ASSERT(pool.n_active == 1, "1 active voice");
    
    int v1 = ytpmv_voice_allocate(&pool, 1, 0, 64, 0.5f, 0.5f, 0.8f, 0.9f);
    ASSERT(v1 >= 0, "Allocated voice 1");
    ASSERT(pool.n_active == 2, "2 active voices");
    
    /* Test 16: Voice pool update */
    ytpmv_voice_pool_update(&pool, 0.6f); /* First voice should be done */
    ASSERT(pool.n_active == 1, "1 voice remaining after update");
    
    /* Test 17: Chord decomposition */
    ytpmv_project proj2;
    ytpmv_project_init(&proj2);
    int pc0 = ytpmv_project_add_character(&proj2, "CharA");
    int pc1 = ytpmv_project_add_character(&proj2, "CharB");
    /* CharA: low range */
    ytpmv_character_add_sample(ytpmv_project_get_character(&proj2, pc0), "low", 1.0f, 0.15f, 220.0f);
    /* CharB: high range */
    ytpmv_character_add_sample(ytpmv_project_get_character(&proj2, pc1), "high", 1.0f, 0.15f, 440.0f);
    
    ytpmv_chord chord;
    chord.notes[0] = 57; /* Low note */
    chord.notes[1] = 69; /* High note */
    chord.n_notes = 2;
    chord.start_time = 0.0f;
    chord.duration = 0.5f;
    
    ytpmv_voice voices[8];
    int n_voices = ytpmv_decompose_chord(&chord, &proj2, voices, 8);
    ASSERT(n_voices == 2, "Decomposed chord into 2 voices");
    ASSERT(voices[0].character_index == 0, "Low note assigned to CharA");
    ASSERT(voices[1].character_index == 1, "High note assigned to CharB");
    
    /* Test 18: Quality evaluation */
    float ratios[] = {1.0f, 1.1f, 0.95f, 1.05f};
    float starts[] = {0.0f, 0.5f, 1.0f, 1.5f};
    float durs[] = {0.4f, 0.4f, 0.4f, 0.4f};
    ytpmv_quality_score score = ytpmv_evaluate(ratios, 4, starts, durs, 120.0f, 44100.0f);
    ASSERT(score.pitch_accuracy > 0.8f, "High pitch accuracy (small shifts)");
    ASSERT(score.timing_accuracy > 0.9f, "High timing accuracy (on beat)");
    ASSERT(score.volume_consistency > 0.9f, "High volume consistency (even durs)");
    ASSERT(score.overall > 0.7f, "Good overall quality");
    printf("  Quality: pitch=%.2f timing=%.2f vol=%.2f formant=%.2f overall=%.2f\n",
           score.pitch_accuracy, score.timing_accuracy, score.volume_consistency,
           score.formant_quality, score.overall);
    
    /* Test 19: Quality evaluation with bad shifts */
    float bad_ratios[] = {0.5f, 2.0f, 0.6f, 1.8f};
    ytpmv_quality_score bad_score = ytpmv_evaluate(bad_ratios, 4, starts, durs, 120.0f, 44100.0f);
    ASSERT(bad_score.pitch_accuracy < score.pitch_accuracy, "Lower pitch accuracy with big shifts");
    ASSERT(bad_score.overall < score.overall, "Lower overall quality with big shifts");
    
    printf("\n=== Multi-Character Engine: %d passed, %d failed ===\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
