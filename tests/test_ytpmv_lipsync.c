/* test_ytpmv_lipsync.c — Lip-sync/viseme engine tests (R131b) */
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

    printf("=== Lip-Sync / Viseme Engine (R131b) ===\n\n");

    /* ---- Engine creation ---- */
    printf("--- Engine Lifecycle ---\n");
    wb_lipsync_engine *eng = wb_lipsync_engine_create(48000.0f, 30.0f);
    CHECK(eng != NULL, "engine_create: non-NULL engine");

    /* ---- Viseme ID constants ---- */
    printf("\n--- Viseme ID Constants ---\n");
    CHECK(WB_VISEME_SIL == 0, "viseme_SIL == 0");
    CHECK(WB_VISEME_PP  == 1, "viseme_PP == 1");
    CHECK(WB_VISEME_FF  == 2, "viseme_FF == 2");
    CHECK(WB_VISEME_TH  == 3, "viseme_TH == 3");
    CHECK(WB_VISEME_DD  == 4, "viseme_DD == 4");
    CHECK(WB_VISEME_KK  == 5, "viseme_KK == 5");
    CHECK(WB_VISEME_CH  == 6, "viseme_CH == 6");
    CHECK(WB_VISEME_SS  == 7, "viseme_SS == 7");
    CHECK(WB_VISEME_NN  == 8, "viseme_NN == 8");
    CHECK(WB_VISEME_RR  == 9, "viseme_RR == 9");
    CHECK(WB_VISEME_AA  == 10, "viseme_AA == 10");
    CHECK(WB_VISEME_EE  == 11, "viseme_EE == 11");
    CHECK(WB_VISEME_IH  == 12, "viseme_IH == 12");
    CHECK(WB_VISEME_OH  == 13, "viseme_OH == 13");
    CHECK(WB_VISEME_OO  == 14, "viseme_OO == 14");

    /* ---- Phoneme-to-viseme mapping ---- */
    printf("\n--- Phoneme-to-Viseme Mapping ---\n");
    CHECK(wb_phoneme_to_viseme(PHON_VOWEL_A) == WB_VISEME_AA, "phoneme A → AA");
    CHECK(wb_phoneme_to_viseme(PHON_VOWEL_E) == WB_VISEME_EE, "phoneme E → EE");
    CHECK(wb_phoneme_to_viseme(PHON_VOWEL_I) == WB_VISEME_IH, "phoneme I → IH");
    CHECK(wb_phoneme_to_viseme(PHON_VOWEL_O) == WB_VISEME_OH, "phoneme O → OH");
    CHECK(wb_phoneme_to_viseme(PHON_VOWEL_U) == WB_VISEME_OO, "phoneme U → OO");
    CHECK(wb_phoneme_to_viseme(PHON_CONSONANT_M) == WB_VISEME_PP, "phoneme M → PP");
    CHECK(wb_phoneme_to_viseme(PHON_CONSONANT_F) == WB_VISEME_FF, "phoneme F → FF");
    CHECK(wb_phoneme_to_viseme(PHON_CONSONANT_S) == WB_VISEME_SS, "phoneme S → SS");
    CHECK(wb_phoneme_to_viseme(PHON_CONSONANT_N) == WB_VISEME_NN, "phoneme N → NN");
    CHECK(wb_phoneme_to_viseme(PHON_CONSONANT_R) == WB_VISEME_RR, "phoneme R → RR");
    CHECK(wb_phoneme_to_viseme(PHON_CONSONANT_D) == WB_VISEME_DD, "phoneme D → DD");
    CHECK(wb_phoneme_to_viseme(PHON_CONSONANT_K) == WB_VISEME_KK, "phoneme K → KK");
    CHECK(wb_phoneme_to_viseme(PHON_CONSONANT_T) == WB_VISEME_TH, "phoneme T → TH");
    CHECK(wb_phoneme_to_viseme(PHON_SILENCE) == WB_VISEME_SIL, "phoneme SIL → SIL");

    /* ---- Viseme names ---- */
    printf("\n--- Viseme Names ---\n");
    CHECK(strcmp(wb_lipsync_viseme_name(WB_VISEME_SIL), "SIL") == 0, "name SIL");
    CHECK(strcmp(wb_lipsync_viseme_name(WB_VISEME_AA), "AA") == 0, "name AA");
    CHECK(strcmp(wb_lipsync_viseme_name(WB_VISEME_OO), "OO") == 0, "name OO");

    /* ---- Mouth shapes ---- */
    printf("\n--- Mouth Shapes ---\n");
    CHECK(strcmp(wb_lipsync_mouth_shape(WB_VISEME_SIL), "rest") == 0, "shape SIL = rest");
    CHECK(strcmp(wb_lipsync_mouth_shape(WB_VISEME_PP), "closed") == 0, "shape PP = closed");
    CHECK(strcmp(wb_lipsync_mouth_shape(WB_VISEME_AA), "open") == 0, "shape AA = open");
    CHECK(strcmp(wb_lipsync_mouth_shape(WB_VISEME_OO), "pucker") == 0, "shape OO = pucker");

    /* ---- Add phonemes and generate timeline ---- */
    printf("\n--- Timeline Generation ---\n");
    wb_lipsync_add_phoneme(eng, PHON_VOWEL_A, 0.0f, 0.2f);
    wb_lipsync_add_phoneme(eng, PHON_CONSONANT_M, 0.2f, 0.3f);
    wb_lipsync_add_phoneme(eng, PHON_VOWEL_E, 0.3f, 0.5f);
    wb_lipsync_add_phoneme(eng, PHON_CONSONANT_S, 0.5f, 0.6f);
    wb_lipsync_add_phoneme(eng, PHON_VOWEL_O, 0.6f, 0.8f);
    wb_lipsync_add_phoneme(eng, PHON_SILENCE, 0.8f, 1.0f);

    int n_frames = wb_lipsync_generate_timeline(eng);
    CHECK(n_frames >= 5, "timeline: at least 5 viseme frames generated");
    CHECK(wb_lipsync_frame_count(eng) == n_frames, "frame_count matches generated count");

    /* ---- Viseme at time queries ---- */
    printf("\n--- Viseme at Time ---\n");
    CHECK(wb_lipsync_viseme_at(eng, 0.1f) == WB_VISEME_AA, "at 0.1s → AA (vowel A)");
    CHECK(wb_lipsync_viseme_at(eng, 0.25f) == WB_VISEME_PP, "at 0.25s → PP (consonant M)");
    CHECK(wb_lipsync_viseme_at(eng, 0.4f) == WB_VISEME_EE, "at 0.4s → EE (vowel E)");
    CHECK(wb_lipsync_viseme_at(eng, 0.55f) == WB_VISEME_SS, "at 0.55s → SS (consonant S)");
    CHECK(wb_lipsync_viseme_at(eng, 0.7f) == WB_VISEME_OH, "at 0.7s → OH (vowel O)");
    CHECK(wb_lipsync_viseme_at(eng, 0.9f) == WB_VISEME_SIL, "at 0.9s → SIL (silence)");

    /* ---- Frame access ---- */
    printf("\n--- Frame Access ---\n");
    const wb_viseme_frame *fr = wb_lipsync_get_frame(eng, 0);
    CHECK(fr != NULL, "get_frame(0): non-NULL");
    CHECK(fr != NULL && fr->viseme_id == WB_VISEME_AA, "frame[0]: first viseme is AA");
    CHECK(fr != NULL && fr->start_time == 0.0f, "frame[0]: starts at 0.0");

    const wb_viseme_frame *fr_last = wb_lipsync_get_frame(eng, n_frames - 1);
    CHECK(fr_last != NULL && fr_last->viseme_id == WB_VISEME_SIL, "last frame: SIL");

    /* Out-of-bounds access returns NULL */
    CHECK(wb_lipsync_get_frame(eng, n_frames) == NULL, "get_frame(past end): NULL");
    CHECK(wb_lipsync_get_frame(eng, -1) == NULL, "get_frame(-1): NULL");

    /* ---- FFmpeg filter generation ---- */
    printf("\n--- FFmpeg Filter ---\n");
    const char *filter = wb_lipsync_generate_ffmpeg_filter(eng);
    CHECK(filter != NULL && strlen(filter) > 0, "ffmpeg_filter: non-empty");
    CHECK(strstr(filter, "select") != NULL, "ffmpeg_filter: contains 'select'");
    CHECK(strstr(filter, "*10") != NULL || strstr(filter, "*0") != NULL,
          "ffmpeg_filter: contains viseme IDs");

    /* ---- Mouth overlay filter ---- */
    printf("\n--- Mouth Overlay Filter ---\n");
    const char *overlay = wb_lipsync_generate_mouth_overlay(eng, "mouths/mouth_");
    CHECK(overlay != NULL && strlen(overlay) > 0, "mouth_overlay: non-empty");
    CHECK(strstr(overlay, "overlay") != NULL, "mouth_overlay: contains 'overlay'");

    /* ---- Blend speed ---- */
    printf("\n--- Blend Speed ---\n");
    wb_lipsync_set_blend_speed(eng, 0.7f);
    /* No crash = pass; we test indirectly via timeline regeneration */
    int n2 = wb_lipsync_generate_timeline(eng);
    CHECK(n2 >= 5, "timeline regen after blend_speed change");

    /* ---- Legacy viseme bridge ---- */
    printf("\n--- Legacy Viseme Bridge ---\n");
    CHECK(wb_lipsync_map_to_legacy_viseme(WB_VISEME_SIL) == VISEME_REST, "bridge SIL → REST");
    CHECK(wb_lipsync_map_to_legacy_viseme(WB_VISEME_AA) == VISEME_AH, "bridge AA → AH");
    CHECK(wb_lipsync_map_to_legacy_viseme(WB_VISEME_EE) == VISEME_EE, "bridge EE → EE");
    CHECK(wb_lipsync_map_to_legacy_viseme(WB_VISEME_OO) == VISEME_OO, "bridge OO → OO");

    /* ---- Load from phoneme DB ---- */
    printf("\n--- Phoneme DB Loading ---\n");
    wb_phoneme_db *db = wb_phoneme_db_create(16);
    wb_phoneme_add(db, 0.0f, 0.3f, 200.0f, 0.8f, PHON_VOWEL_O);
    wb_phoneme_add(db, 0.3f, 0.5f, 400.0f, 0.6f, PHON_CONSONANT_K);
    wb_phoneme_add(db, 0.5f, 0.8f, 600.0f, 0.9f, PHON_VOWEL_U);

    wb_lipsync_engine *eng2 = wb_lipsync_engine_create(44100.0f, 24.0f);
    int loaded = wb_lipsync_load_phonemes(eng2, db);
    CHECK(loaded == 3, "load_phonemes: loaded 3 phonemes");

    int n_db = wb_lipsync_generate_timeline(eng2);
    CHECK(n_db >= 3, "timeline from DB: at least 3 frames");
    CHECK(wb_lipsync_viseme_at(eng2, 0.15f) == WB_VISEME_OH, "DB timeline: OH at 0.15s");
    CHECK(wb_lipsync_viseme_at(eng2, 0.4f) == WB_VISEME_KK, "DB timeline: KK at 0.4s");
    CHECK(wb_lipsync_viseme_at(eng2, 0.65f) == WB_VISEME_OO, "DB timeline: OO at 0.65f");

    /* ---- Edge cases ---- */
    printf("\n--- Edge Cases ---\n");
    CHECK(wb_lipsync_viseme_at(NULL, 0.5f) == WB_VISEME_SIL, "NULL engine → SIL");
    CHECK(wb_phoneme_to_viseme(PHON_UNKNOWN) == WB_VISEME_SIL, "unknown phoneme → SIL");

    /* Time before first frame */
    CHECK(wb_lipsync_viseme_at(eng, -1.0f) == WB_VISEME_SIL, "negative time → SIL");

    /* ---- Cleanup ---- */
    wb_lipsync_engine_destroy(eng);
    wb_lipsync_engine_destroy(eng2);
    wb_phoneme_db_free(db);

    printf("\n=== Results: %d PASS, %d FAIL ===\n", p, f);
    return f > 0 ? 1 : 0;
}