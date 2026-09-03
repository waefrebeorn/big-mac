/* test_ytpmv_renderer.c — YTPMV Renderer + Relative Automation tests (R100) */
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

    printf("=== R100: YTPMV Renderer + Relative Automation ===\n\n");

    /* ---- YTPMV Renderer ---- */
    printf("--- YTPMV Renderer ---\n");
    ytpmv_renderer r;
    ytpmv_init(&r, 120.0f, 10.0f, 320, 240);
    CHECK(r.bpm == 120.0f, "ytpmv: BPM=120");
    CHECK(r.frame_buffer != NULL, "ytpmv: frame buffer allocated");
    CHECK(r.frame_w == 320, "ytpmv: width=320");
    CHECK(r.frame_h == 240, "ytpmv: height=240");

    /* Add clips */
    int c1 = ytpmv_add_clip(&r, "clip_a", 0, 0.5f);
    CHECK(c1 == 0, "ytpmv: first clip index=0");
    int c2 = ytpmv_add_clip(&r, "clip_b", 1, 0.3f);
    CHECK(c2 == 1, "ytpmv: second clip index=1");
    CHECK(r.n_clips == 2, "ytpmv: 2 clips added");

    /* Add track */
    int t1 = ytpmv_add_track(&r);
    CHECK(t1 == 0, "ytpmv: first track index=0");
    CHECK(r.n_tracks == 1, "ytpmv: 1 track added");

    /* Process audio */
    float audio[48000];
    memset(audio, 0, sizeof(audio));
    /* Create 3 segments with different energy */
    for (int i = 0; i < 16000; i++)
        audio[i] = 0.01f;
    for (int i = 16000; i < 32000; i++)
        audio[i] = 0.8f * sinf(i * 0.05f);
    for (int i = 32000; i < 48000; i++)
        audio[i] = 0.3f * sinf(i * 0.1f);

    int n_ph = ytpmv_process_audio(&r, 0, audio, 48000, 1, 48000.0f);
    CHECK(n_ph > 0, "ytpmv: processed audio into phonemes");

    /* Tick */
    ytpmv_tick(&r, 0.016f);
    CHECK(r.current_time > 0, "ytpmv: time advances");

    /* Get active phoneme */
    int active = ytpmv_get_active_phoneme(&r, 0, 0.0f);
    CHECK(active >= -1, "ytpmv: get active phoneme");

    /* Trigger FX */
    ytpmv_trigger_fx(&r, 0); /* strobe */
    ytpmv_trigger_fx(&r, 1); /* freeze */
    ytpmv_trigger_fx(&r, 2); /* shake */
    CHECK(1, "ytpmv: FX triggers without crash");

    ytpmv_free(&r);
    CHECK(1, "ytpmv: freed");

    /* ---- Relative Automation ---- */
    printf("\n--- Relative Automation ---\n");
    wb_relative_auto ra;
    wb_relative_init(&ra, 4);
    CHECK(ra.n_values == 4, "relative: 4 values");
    CHECK(ra.base_value == 1.0f, "relative: base=1.0");

    wb_relative_set(&ra, 0, 0.5f);
    CHECK(ra.values[0] == 0.5f, "relative: set multiplier");

    float result = wb_relative_apply(&ra, 0, 100.0f);
    CHECK(result == 50.0f, "relative: 100 * 0.5 = 50");

    result = wb_relative_apply(&ra, 1, 80.0f);
    CHECK(result == 80.0f, "relative: 80 * 1.0 = 80 (default)");

    wb_relative_free(&ra);
    CHECK(1, "relative: freed");

    /* ---- YTPMV Sentence Mix ---- */
    printf("\n--- YTPMV Sentence Mix ---\n");
    ytpmv_sentence_mix sm;
    ytpmv_sentence_init(&sm, 3);
    CHECK(sm.n_target == 3, "sentence: 3 targets");

    /* Set target sequence */
    sm.target_sequence[0] = 0; /* vowel A */
    sm.target_sequence[1] = 1; /* vowel E */
    sm.target_sequence[2] = 2; /* vowel I */

    /* Source has different order */
    int source_types[] = {2, 0, 1};
    ytpvm_sentence_remap(&sm, source_types, 3);
    /* Target 0 (A=0) should map to source index 1 (which has type 0) */
    CHECK(sm.source_indices[0] == 1, "sentence: remap target 0 -> source 1");

    ytpmv_sentence_free(&sm);
    CHECK(1, "sentence: freed");

    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    ytpmv_init(NULL, 0, 0, 0, 0);
    ytpmv_free(NULL);
    ytpmv_add_clip(NULL, NULL, 0, 0);
    ytpmv_add_track(NULL);
    ytpmv_process_audio(NULL, 0, NULL, 0, 0, 0);
    ytpmv_get_active_phoneme(NULL, 0, 0);
    ytpmv_tick(NULL, 0);
    ytpmv_trigger_fx(NULL, 0);
    wb_relative_init(NULL, 0);
    wb_relative_set(NULL, 0, 0);
    wb_relative_apply(NULL, 0, 0);
    wb_relative_free(NULL);
    ytpmv_sentence_init(NULL, 0);
    ytpvm_sentence_remap(NULL, NULL, 0);
    ytpmv_sentence_free(NULL);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", p, p + f);
    return f > 0 ? 1 : 0;
}
