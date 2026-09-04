/* test_sampler_inst.c — Sampler Instrument + Piano Roll tests (R102) */
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

    printf("=== R102: Sampler Instrument + Piano Roll ===\n\n");

    /* ---- Sampler Instrument ---- */
    printf("--- Sampler Instrument ---\n");
    wb_sampler_inst s;
    wb_sampler_init(&s);
    CHECK(s.volume == 1.0f, "sampler: default volume=1.0");
    CHECK(s.n_regions == 0, "sampler: starts empty");

    /* Create a test sample: 1kHz sine, 4800 frames @ 48kHz */
    int n_frames = 4800;
    float *sample = (float *)malloc(n_frames * sizeof(float));
    for (int i = 0; i < n_frames; i++)
        sample[i] = sinf(i * 2.0f * M_PI * 1000.0f / 48000.0f);

    int region = wb_sampler_add_region(&s, sample, n_frames, 1, 60, "phoneme_a");
    CHECK(region == 0, "sampler: first region index=0");
    CHECK(s.n_regions == 1, "sampler: 1 region added");
    CHECK(strcmp(s.regions[0].name, "phoneme_a") == 0, "sampler: region name");

    /* Play the sample at root note (should be original pitch) */
    float output[4800] = {0};
    int written = wb_sampler_play_note(&s, 60, 1.0f, output, 4800, 1, 48000.0f);
    CHECK(written > 0, "sampler: played note produces output");

    /* Check that output has content */
    float max_val = 0;
    for (int i = 0; i < written; i++)
        if (fabsf(output[i]) > max_val) max_val = fabsf(output[i]);
    CHECK(max_val > 0.1f, "sampler: output has signal");

    /* Play at higher note (should be shorter due to pitch shift) */
    float output2[4800] = {0};
    int written2 = wb_sampler_play_note(&s, 72, 1.0f, output2, 4800, 1, 48000.0f);
    CHECK(written2 > 0, "sampler: played higher note");
    CHECK(written2 < written, "sampler: higher note = shorter (pitch up)");

    /* Velocity check */
    float output3[4800] = {0};
    wb_sampler_play_note(&s, 60, 0.5f, output3, 4800, 1, 48000.0f);
    float max3 = 0;
    for (int i = 0; i < 4800; i++)
        if (fabsf(output3[i]) > max3) max3 = fabsf(output3[i]);
    CHECK(max3 < max_val, "sampler: lower velocity = quieter");

    wb_sampler_free(&s);
    free(sample);
    CHECK(1, "sampler: freed");

    /* ---- Piano Roll ---- */
    printf("\n--- Piano Roll ---\n");
    wb_piano_roll pr;
    wb_piano_init(&pr, 120.0f, 4, 4);
    CHECK(pr.bpm == 120.0f, "piano: BPM=120");
    CHECK(pr.beats_per_bar == 4, "piano: 4/4 time");
    CHECK(pr.bars == 4, "piano: 4 bars");

    /* Add notes */
    int n1 = wb_piano_add_note(&pr, 0, 0, 0, 60, 1.0f, TICKS_PER_BEAT);
    CHECK(n1 == 0, "piano: first note index=0");
    int n2 = wb_piano_add_note(&pr, 0, 1, 0, 64, 0.8f, TICKS_PER_BEAT);
    CHECK(n2 == 1, "piano: second note index=1");
    int n3 = wb_piano_add_note(&pr, 0, 2, 0, 67, 0.9f, TICKS_PER_BEAT);
    CHECK(n3 == 2, "piano: third note index=2");
    CHECK(pr.n_notes == 3, "piano: 3 notes added");

    /* Get notes at a tick */
    int indices[16];
    int count = wb_piano_get_notes_at(&pr, 0, indices, 16);
    CHECK(count >= 1, "piano: found notes at tick 0");

    /* Transpose */
    wb_piano_transpose(&pr, 2);
    CHECK(pr.notes[0].note == 62, "piano: transposed +2 semitones");

    /* Total ticks */
    int total = wb_piano_total_ticks(&pr);
    CHECK(total == 4 * 4 * TICKS_PER_BEAT, "piano: total ticks");

    /* Quantize test: use init's default grid (4 = 16th notes) */
    wb_piano_roll pr2;
    wb_piano_init(&pr2, 120.0f, 4, 1);
    /* quantize_grid=4 (16th), quantize_strength=100 by default */
    /* Add note at tick 50 (should snap to 0 since 50 < 60) */
    int qn = wb_piano_add_note(&pr2, 0, 0, 50, 60, 1.0f, TICKS_PER_BEAT);
    int snapped_tick = pr2.notes[qn].tick_position;
    /* With 16th-note grid (120 ticks), 50 should snap to 0 */
    CHECK(snapped_tick == 0, "piano: quantized 50->0");

    /* ---- Clip Triggers ---- */
    printf("\n--- Clip Triggers ---\n");
    wb_clip_triggers ct;
    wb_clip_triggers_init(&ct);
    wb_clip_triggers_map(&ct, 60, 0, 0.0f);
    wb_clip_triggers_map(&ct, 64, 1, -12.0f);
    wb_clip_triggers_map(&ct, 67, 2, 0.0f);

    float pitch_shift;
    int clip = wb_clip_triggers_find(&ct, 60, &pitch_shift);
    CHECK(clip == 0, "clips: note 60 -> clip 0");
    clip = wb_clip_triggers_find(&ct, 64, &pitch_shift);
    CHECK(clip == 1, "clips: note 64 -> clip 1");
    CHECK(pitch_shift == -12.0f, "clips: note 64 pitch shift -12");

    /* ---- Harmony Generator ---- */
    printf("\n--- Harmony Generator ---\n");
    wb_harmony_gen hg;
    wb_harmony_init(&hg);
    CHECK(hg.n_voices == 1, "harmony: default 1 voice");
    CHECK(hg.voices[0].interval_semitones == 4, "harmony: default third");

    wb_harmony_add_voice(&hg, 7, 0.7f); /* fifth */
    CHECK(hg.n_voices == 2, "harmony: added fifth");

    /* Generate harmony from source piano roll */
    wb_piano_roll source;
    wb_piano_init(&source, 120.0f, 4, 1);
    wb_piano_add_note(&source, 0, 0, 0, 60, 1.0f, TICKS_PER_BEAT);
    wb_piano_add_note(&source, 0, 1, 0, 64, 0.8f, TICKS_PER_BEAT);

    wb_piano_roll harmony;
    wb_piano_init(&harmony, 120.0f, 4, 1);
    int added = wb_harmony_generate(&hg, &source, &harmony);
    CHECK(added == 4, "harmony: generated 4 notes (2 melody x 2 voices)");

    /* ---- Stutter Retrigger ---- */
    printf("\n--- Stutter Retrigger ---\n");
    wb_stutter_cfg stutter;
    stutter.retrigger_count = 4;
    stutter.retrigger_div = 4; /* 16th notes */
    stutter.velocity_decay = 0.8f;

    wb_piano_roll stutter_output;
    wb_piano_init(&stutter_output, 120.0f, 4, 1);
    stutter_output.quantize_strength = 0; /* disable quantize for precise stutter */
    wb_piano_note source_note;
    memset(&source_note, 0, sizeof(source_note));
    source_note.active = 1;
    source_note.note = 60;
    source_note.velocity = 1.0f;
    source_note.length = TICKS_PER_BEAT;
    int stutters = wb_stutter_apply(&stutter, &source_note, &stutter_output, 0, 0, 0);
    CHECK(stutters == 4, "stutter: 4 retriggers");

    /* ---- Bass Drop ---- */
    printf("\n--- Bass Drop ---\n");
    wb_bass_drop_cfg bass_cfg;
    wb_bass_drop_init(&bass_cfg);
    CHECK(bass_cfg.bass_threshold == 48, "bass: threshold=C3");

    wb_piano_roll bass_roll;
    wb_piano_init(&bass_roll, 120.0f, 4, 1);
    wb_piano_add_note(&bass_roll, 0, 0, 0, 36, 1.0f, TICKS_PER_BEAT); /* C2 = bass */
    wb_piano_add_note(&bass_roll, 0, 1, 0, 60, 0.8f, TICKS_PER_BEAT); /* C4 = not bass */

    int bass_processed = wb_bass_drop_process(&bass_cfg, &bass_roll);
    CHECK(bass_processed == 1, "bass: processed 1 bass note");
    CHECK(bass_roll.notes[0].note == 24, "bass: C2 dropped to C1");
    CHECK(bass_roll.notes[1].note == 60, "bass: non-bass unchanged");

    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    wb_sampler_init(NULL);
    wb_sampler_add_region(NULL, NULL, 0, 0, 0, NULL);
    wb_sampler_play_note(NULL, 0, 0, NULL, 0, 0, 0);
    wb_sampler_free(NULL);
    wb_piano_init(NULL, 0, 0, 0);
    wb_piano_add_note(NULL, 0, 0, 0, 0, 0, 0);
    wb_piano_get_notes_at(NULL, 0, NULL, 0);
    wb_piano_dedupe(NULL);
    wb_piano_transpose(NULL, 0);
    wb_clip_triggers_init(NULL);
    wb_clip_triggers_map(NULL, 0, 0, 0);
    wb_clip_triggers_find(NULL, 0, NULL);
    wb_harmony_init(NULL);
    wb_harmony_add_voice(NULL, 0, 0);
    wb_harmony_generate(NULL, NULL, NULL);
    wb_bass_drop_init(NULL);
    wb_bass_drop_process(NULL, NULL);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", p, p + f);
    return f > 0 ? 1 : 0;
}
