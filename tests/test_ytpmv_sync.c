/* test_ytpmv_sync.c — YTPMV sync engine tests (R095) */
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

    printf("=== YTPMV Sync Engine (R095) ===\n\n");

    /* ---- Beat Detection ---- */
    printf("--- Beat Detection ---\n");
    /* Generate a simple beat pattern: 120 BPM = 2 beats/sec */
    int sr = 48000;
    int dur = 4; /* 4 seconds */
    int n_frames = sr * dur;
    float *audio = (float *)calloc(n_frames, sizeof(float));

    /* Create beats at 120 BPM (every 0.5s) with a short noise burst */
    float bpm = 120.0f;
    float beat_interval = 60.0f / bpm;
    for (int b = 0; b < dur * 2; b++) {
        int center = (int)(b * beat_interval * sr);
        int burst_len = (int)(sr * 0.05f); /* 50ms burst */
        for (int i = 0; i < burst_len && center + i < n_frames; i++) {
            audio[center + i] = 0.8f * (1.0f - (float)i / burst_len);
        }
    }

    wb_beat_map *beats = wb_beat_map_create(64);
    int n_beats = wb_ytpmv_detect_beats(audio, n_frames, 1, sr, 0.3f, beats);
    CHECK(n_beats > 0, "beat_detect: beats found");
    CHECK(beats->bpm > 80 && beats->bpm < 160, "beat_detect: BPM in range");

    /* ---- Audio to Keyframes ---- */
    printf("\n--- Audio to Keyframes ---\n");
    wb_audio_keys *keys = wb_audio_keys_create(1024);
    int n_keys = wb_audio_to_keyframes(audio, n_frames, 1, sr, 0.04f, keys);
    CHECK(n_keys > 0, "audio_to_keys: keyframes generated");
    CHECK(keys->values[0] >= 0.0f && keys->values[0] <= 1.0f, "audio_to_keys: values normalized");

    /* ---- Wiggle Expression ---- */
    printf("\n--- Wiggle Expression ---\n");
    float *wiggle = (float *)calloc(4800, sizeof(float));
    wb_wiggle(wiggle, 4800, sr, 5.0f, 1.0f, 42);
    float wiggle_max = 0;
    for (int i = 0; i < 4800; i++)
        if (fabsf(wiggle[i]) > wiggle_max) wiggle_max = fabsf(wiggle[i]);
    CHECK(wiggle_max > 0.1f, "wiggle: produces movement");
    CHECK(wiggle_max <= 1.5f, "wiggle: bounded amplitude");

    /* Audio-driven wiggle */
    float *audio_wiggle = (float *)calloc(4800, sizeof(float));
    wb_audio_wiggle(audio_wiggle, 4800, sr, 5.0f, 1.0f, keys->values, n_keys, 0.04f);
    float aw_max = 0;
    for (int i = 0; i < 4800; i++)
        if (fabsf(audio_wiggle[i]) > aw_max) aw_max = fabsf(audio_wiggle[i]);
    CHECK(aw_max > 0.0f, "audio_wiggle: produces movement");

    /* ---- Lip Sync ---- */
    printf("\n--- Lip Sync ---\n");
    wb_phoneme_db *db = wb_phoneme_db_create(16);
    wb_phoneme_add(db, 0.0f, 0.5f, 300.0f, 0.8f, PHON_VOWEL_A);
    wb_phoneme_add(db, 0.5f, 1.0f, 500.0f, 0.6f, PHON_VOWEL_E);
    wb_phoneme_add(db, 1.0f, 1.5f, 150.0f, 0.7f, PHON_VOWEL_O);
    wb_phoneme_add(db, 1.5f, 2.0f, 0.0f, 0.0f, PHON_SILENCE);
    wb_phoneme_add(db, 2.0f, 2.5f, 700.0f, 0.9f, PHON_CONSONANT_M);

    wb_lip_frame lip_frames[16];
    int n_lip = wb_generate_lip_sync(db, lip_frames, 16);
    CHECK(n_lip >= 3, "lip_sync: at least 3 visemes");

    /* Check viseme mapping */
    wb_viseme v0 = phoneme_to_viseme(PHON_VOWEL_A);
    CHECK(v0 == VISEME_AH, "viseme_map: A→AH");
    wb_viseme v1 = phoneme_to_viseme(PHON_VOWEL_E);
    CHECK(v1 == VISEME_EE, "viseme_map: E→EE");
    wb_viseme v2 = phoneme_to_viseme(PHON_CONSONANT_M);
    CHECK(v2 == VISEME_MBP, "viseme_map: M→MBP");
    wb_viseme v3 = phoneme_to_viseme(PHON_SILENCE);
    CHECK(v3 == VISEME_REST, "viseme_map: silence→REST");

    /* ---- Beat-Synced Video ---- */
    printf("\n--- Beat-Synced Video ---\n");
    /* Use 2 seconds of zoom curve (matches beat positions) */
    int zoom_frames = sr * 2;
    float *zoom_curve = (float *)calloc(zoom_frames, sizeof(float));
    wb_beat_sync_zoom(zoom_curve, zoom_frames, sr, beats, 0.5f);
    /* Check that zoom curve has variation (some frames zoomed, some not) */
    float zoom_min = 999, zoom_max = 0;
    for (int i = 0; i < zoom_frames; i++) {
        if (zoom_curve[i] < zoom_min) zoom_min = zoom_curve[i];
        if (zoom_curve[i] > zoom_max) zoom_max = zoom_curve[i];
    }
    CHECK(zoom_max > zoom_min + 0.01f, "beat_zoom: zoom variation detected");

    uint8_t *frame = (uint8_t *)calloc(64 * 64 * 4, 1);
    for (int i = 0; i < 64 * 64; i++) {
        frame[i*4] = 128; frame[i*4+1] = 128; frame[i*4+2] = 128; frame[i*4+3] = 255;
    }
    wb_beat_sync_flash(frame, 64, 64, (int)(beat_interval * sr), sr, beats, 0.5f);
    CHECK(frame[0*4] > 128 || frame[0*4+1] > 128 || frame[0*4+2] > 128,
          "beat_flash: flash brightens pixels");

    /* ---- YTPMV Auto-Pilot ---- */
    printf("\n--- YTPMV Auto-Pilot ---\n");
    wb_ytpmv_plan *plan = wb_ytpmv_plan_create();
    CHECK(plan != NULL, "plan: created");

    int rc = wb_ytpmv_analyze(plan, audio, n_frames, 1, sr);
    CHECK(rc == 0, "plan: analysis completed");
    CHECK(plan->beats->n_beats > 0, "plan: beats detected in plan");
    CHECK(plan->amp_keys->n_keyframes > 0, "plan: amplitude keyframes generated");
    CHECK(plan->phonemes->count > 0, "plan: phonemes detected");
    CHECK(plan->n_lip_frames > 0, "plan: lip sync frames generated");

    wb_ytpmv_plan_free(plan);
    CHECK(1, "plan: freed");

    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    wb_ytpmv_detect_beats(NULL, 0, 0, 0, 0, NULL);
    wb_audio_to_keyframes(NULL, 0, 0, 0, 0, NULL);
    wb_wiggle(NULL, 0, 0, 0, 0, 0);
    wb_audio_wiggle(NULL, 0, 0, 0, 0, NULL, 0, 0);
    wb_beat_sync_zoom(NULL, 0, 0, NULL, 0);
    wb_beat_sync_flash(NULL, 0, 0, 0, 0, NULL, 0);
    wb_ytpmv_plan_free(NULL);
    wb_beat_map_free(NULL);
    wb_audio_keys_free(NULL);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", p, p + f);

    free(audio);
    free(wiggle);
    free(audio_wiggle);
    free(zoom_curve);
    free(frame);
    wb_beat_map_free(beats);
    wb_audio_keys_free(keys);
    wb_phoneme_db_free(db);

    return f > 0 ? 1 : 0;
}
