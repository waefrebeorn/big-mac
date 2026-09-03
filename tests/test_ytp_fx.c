/* test_ytp_fx.c — YTP effects engine tests (R094) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "wbus/wbus_compositor.h"
#include "wbus/wbus_param_track.h"
}

#define CHECK(cond, msg) do { \
    if (cond) { passed++; printf("  PASS: %s\n", msg); } \
    else { failed++; printf("  FAIL: %s\n", msg); } \
} while(0)

int main(void) {
    int passed = 0, failed = 0;
    uint8_t *img, *out;
    int w = 64, h = 64;
    int sz = w * h * 4;

    printf("=== YTP Effects Engine (R094) ===\n\n");

    /* ---- Keyframe Interpolation (15 types) ---- */
    printf("--- Keyframe Interpolation ---\n");
    CHECK(kf_interpolate(0.0f, WB_KF_LINEAR, 0,0,0) == 0.0f, "linear(0)=0");
    CHECK(kf_interpolate(1.0f, WB_KF_LINEAR, 0,0,0) == 1.0f, "linear(1)=1");
    CHECK(fabsf(kf_interpolate(0.5f, WB_KF_LINEAR, 0,0,0) - 0.5f) < 0.01f, "linear(0.5)=0.5");

    CHECK(kf_interpolate(0.0f, WB_KF_EASE_IN, 0,0,0) == 0.0f, "ease_in(0)=0");
    CHECK(fabsf(kf_interpolate(0.5f, WB_KF_EASE_IN, 0,0,0) - 0.25f) < 0.01f, "ease_in(0.5)=0.25");

    CHECK(kf_interpolate(0.0f, WB_KF_EASE_OUT, 0,0,0) == 0.0f, "ease_out(0)=0");
    CHECK(fabsf(kf_interpolate(0.5f, WB_KF_EASE_OUT, 0,0,0) - 0.75f) < 0.01f, "ease_out(0.5)=0.75");

    CHECK(kf_interpolate(0.0f, WB_KF_EASE_INOUT, 0,0,0) == 0.0f, "ease_inout(0)=0");
    CHECK(fabsf(kf_interpolate(0.5f, WB_KF_EASE_INOUT, 0,0,0) - 0.5f) < 0.01f, "ease_inout(0.5)=0.5");

    CHECK(kf_interpolate(0.0f, WB_KF_HOLD, 0,0,0) == 0.0f, "hold(0)=0");
    CHECK(kf_interpolate(0.49f, WB_KF_HOLD, 0,0,0) == 0.0f, "hold(0.49)=0");
    CHECK(kf_interpolate(0.5f, WB_KF_HOLD, 0,0,0) == 1.0f, "hold(0.5)=1");

    float elastic_val = kf_interpolate(1.0f, WB_KF_ELASTIC, 0,0,0);
    CHECK(fabsf(elastic_val - 1.0f) < 0.05f, "elastic(1)≈1");

    float bounce_val = kf_interpolate(1.0f, WB_KF_BOUNCE, 0,0,0);
    CHECK(fabsf(bounce_val - 1.0f) < 0.05f, "bounce(1)≈1");

    float back_val = kf_interpolate(1.0f, WB_KF_BACK, 0,0,0);
    CHECK(fabsf(back_val - 1.0f) < 0.05f, "back(1)≈1");

    float exp_val = kf_interpolate(1.0f, WB_KF_EXPONENTIAL, 0,0,0);
    CHECK(fabsf(exp_val - 1.0f) < 0.05f, "exp(1)≈1");

    float log_val = kf_interpolate(0.0f, WB_KF_LOGARITHMIC, 0,0,0);
    CHECK(log_val == 0.0f, "log(0)=0");

    float scurve_val = kf_interpolate(0.5f, WB_KF_SCURVE, 0,0,0);
    CHECK(fabsf(scurve_val - 0.5f) < 0.05f, "scurve(0.5)≈0.5");

    float tcb_val = kf_interpolate(0.5f, WB_KF_TCB, 0.0f, 0.0f, 0.0f);
    CHECK(tcb_val >= 0.0f && tcb_val <= 1.0f, "tcb(0.5) in [0,1]");

    float bez_val = kf_interpolate(0.0f, WB_KF_BEZIER, 0.25f, 0.75f, 0.0f);
    CHECK(bez_val == 0.0f, "bezier(0)=0");

    /* ---- Cookie Cutter ---- */
    printf("\n--- Cookie Cutter Masks ---\n");
    img = (uint8_t *)calloc(sz, 1);
    out = (uint8_t *)calloc(sz, 1);
    /* Fill with white opaque */
    for (int i = 0; i < w*h; i++) {
        img[i*4+0] = 255; img[i*4+1] = 255;
        img[i*4+2] = 255; img[i*4+3] = 255;
    }
    wb_cookie_cutter(img, w, h, WB_MASK_CIRCLE, 0.5f, 0.5f, 0.3f);
    /* Center pixel should be opaque, corner should be transparent */
    int center_idx = (h/2 * w + w/2) * 4;
    int corner_idx = 0;
    CHECK(img[center_idx+3] == 255, "circle: center opaque");
    CHECK(img[corner_idx+3] == 0, "circle: corner transparent");

    /* Reset and test star */
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    wb_cookie_cutter(img, w, h, WB_MASK_STAR, 0.5f, 0.5f, 0.4f);
    CHECK(img[center_idx+3] == 255, "star: center opaque");
    CHECK(img[corner_idx+3] == 0, "star: corner transparent");

    /* Reset and test heart */
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    wb_cookie_cutter(img, w, h, WB_MASK_HEART, 0.5f, 0.6f, 0.3f);
    CHECK(img[center_idx+3] == 255, "heart: center opaque");

    /* Reset and test diamond */
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    wb_cookie_cutter(img, w, h, WB_MASK_DIAMOND, 0.5f, 0.5f, 0.3f);
    CHECK(img[center_idx+3] == 255, "diamond: center opaque");
    CHECK(img[corner_idx+3] == 0, "diamond: corner transparent");

    /* ---- Mirror / Kaleidoscope ---- */
    printf("\n--- Mirror / Kaleidoscope ---\n");
    memset(img, 0, sz);
    /* Put red dot in top-left */
    img[0] = 255; img[3] = 255;
    wb_mirror_quad(out, img, w, h);
    /* Top-right should have red (horizontal mirror) */
    int tr_idx = (0 * w + (w-1)) * 4;
    CHECK(out[tr_idx] == 255, "mirror: top-right has red");
    /* Bottom-left should have red (vertical mirror) */
    int bl_idx = ((h-1) * w + 0) * 4;
    CHECK(out[bl_idx] == 255, "mirror: bottom-left has red");

    /* Fill image with gradient for kaleidoscope test */
    for (int i = 0; i < w*h; i++) {
        img[i*4+0] = (uint8_t)(i % 256);
        img[i*4+1] = (uint8_t)((i*2) % 256);
        img[i*4+2] = (uint8_t)((i*3) % 256);
        img[i*4+3] = 255;
    }
    wb_kaleidoscope(out, img, w, h, 6);
    CHECK(out[center_idx+3] == 255, "kaleidoscope: center has content");

    /* ---- Geometric Warps ---- */
    printf("\n--- Geometric Warps ---\n");
    memset(img, 0, sz);
    for (int i = 0; i < w*h; i++) {
        img[i*4+0] = (uint8_t)(i % 256);
        img[i*4+3] = 255;
    }
    wb_swirl(out, img, w, h, 90.0f, 0.5f);
    CHECK(out[center_idx+3] == 255, "swirl: output has content");

    wb_spherize(out, img, w, h, 0.5f, 0.5f);
    CHECK(out[center_idx+3] == 255, "spherize: output has content");

    wb_wave_displace(out, img, w, h, 5.0f, 4.0f, 0.0f, 3.0f, 3.0f, 0.0f);
    CHECK(out[center_idx+3] == 255, "wave: output has content");

    /* ---- Zoom Punch / Impact Frame ---- */
    printf("\n--- Zoom Punch / Impact ---\n");
    memset(img, 128, sz);
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    wb_zoom_punch(out, img, w, h, 2.0f);
    CHECK(out[center_idx+3] == 255, "zoom_punch: output has content");

    memset(img, 128, sz);
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    wb_impact_frame(img, w, h, 1);
    CHECK(img[0] == 255 && img[1] == 255 && img[2] == 255, "impact frame: white");
    wb_impact_frame(img, w, h, 0);
    CHECK(img[0] == 0 && img[1] == 0 && img[2] == 0, "impact frame: black");

    /* ---- Scramble ---- */
    printf("\n--- Scramble ---\n");
    memset(img, 0, sz);
    for (int i = 0; i < w*h; i++) {
        img[i*4+0] = (uint8_t)(i % 256);
        img[i*4+3] = 255;
    }
    wb_scramble(out, img, w, h, 42, 8);
    CHECK(out[center_idx+3] == 255, "scramble: output has content");
    /* Verify pixels are shuffled (not identical to input) */
    int same = 1;
    for (int i = 0; i < 100; i++) {
        if (img[i*4] != out[i*4]) { same = 0; break; }
    }
    CHECK(!same, "scramble: pixels are shuffled");

    /* ---- Strobe ---- */
    printf("\n--- Strobe ---\n");
    memset(img, 200, sz);
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    wb_strobe(out, img, w, h, 0, 4, 255, 0, 0);
    CHECK(out[0] == 200, "strobe: even frame = original");
    wb_strobe(out, img, w, h, 5, 4, 255, 0, 0);
    CHECK(out[0] == 255 && out[1] == 0 && out[2] == 0, "strobe: odd frame = red");

    /* ---- CRT ---- */
    printf("\n--- CRT Effect ---\n");
    memset(img, 128, sz);
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    wb_crt_effect(out, img, w, h, 0.3f, 0.01f);
    CHECK(out[center_idx+3] == 255, "crt: output has content");
    /* Even rows should be darker than odd (scanlines) */
    int even_row_pixels = 0, odd_row_pixels = 0;
    for (int x = 0; x < w; x++) {
        even_row_pixels += out[(0 * w + x) * 4];
        odd_row_pixels += out[(1 * w + x) * 4];
    }
    CHECK(even_row_pixels < odd_row_pixels, "crt: scanlines darken even rows");

    /* ---- Recursion (Droste) ---- */
    printf("\n--- Recursion ---\n");
    memset(img, 128, sz);
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    img[0] = 255; /* marker pixel */
    wb_recursion(out, img, w, h, 0.3f, 0.5f, 0.5f, 1);
    CHECK(out[center_idx+3] == 255, "recursion: output has content");

    /* ---- Video Stutter ---- */
    printf("\n--- Video Stutter ---\n");
    memset(img, 128, sz);
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    uint8_t *stutter_out = (uint8_t *)malloc(sz * 4);
    int n = wb_video_stutter(img, w, h, stutter_out, 4);
    CHECK(n == 4, "stutter: 4 frames output");
    /* All frames should be identical */
    int identical = 1;
    for (int f = 1; f < 4; f++) {
        if (memcmp(stutter_out, stutter_out + f * sz, sz) != 0) {
            identical = 0; break;
        }
    }
    CHECK(identical, "stutter: all frames identical");

    /* ---- PiP ---- */
    printf("\n--- Picture-in-Picture ---\n");
    uint8_t *small_img = (uint8_t *)calloc(32*32*4, 1);
    memset(small_img, 255, 32*32*4);
    for (int i = 0; i < 32*32; i++) small_img[i*4+3] = 255;
    memset(out, 0, sz);
    wb_pip_overlay(out, w, h, small_img, 32, 32, 10, 10, 1.0f);
    /* Pixel at (10,10) should be blended with white */
    int pip_idx = (10 * w + 10) * 4;
    CHECK(out[pip_idx+3] == 255, "pip: overlay pixel visible");

    /* ---- Ken Burns ---- */
    printf("\n--- Ken Burns ---\n");
    memset(img, 128, sz);
    for (int i = 0; i < w*h; i++) img[i*4+3] = 255;
    wb_ken_burns(out, img, w, h, 0.5f, 0.3f, 0.3f, 1.0f, 0.7f, 0.7f, 2.0f);
    CHECK(out[center_idx+3] == 255, "ken_burns: output has content");

    /* ---- MIDI Step Sequencer ---- */
    printf("\n--- MIDI Step Sequencer ---\n");
    wb_step_seq seq;
    wb_step_seq_init(&seq, 16);
    wb_step_seq_set(&seq, 0, 1);
    wb_step_seq_set(&seq, 4, 1);
    wb_step_seq_set(&seq, 8, 1);
    wb_step_seq_set(&seq, 12, 1);
    wb_step_seq_start(&seq);
    CHECK(wb_step_seq_tick(&seq) == 1, "step_seq: step 0 fires");
    CHECK(wb_step_seq_tick(&seq) == 0, "step_seq: step 1 silent");
    CHECK(wb_step_seq_tick(&seq) == 0, "step_seq: step 2 silent");
    CHECK(wb_step_seq_tick(&seq) == 0, "step_seq: step 3 silent");
    CHECK(wb_step_seq_tick(&seq) == 1, "step_seq: step 4 fires");

    /* ---- Euclidean Rhythm ---- */
    printf("\n--- Euclidean Rhythm ---\n");
    int pattern[16];
    wb_euclidean_rhythm(pattern, 16, 5);
    int hits = 0;
    for (int i = 0; i < 16; i++) hits += pattern[i];
    CHECK(hits == 5, "euclidean: 5 hits in 16 steps");

    wb_euclidean_rhythm(pattern, 8, 3);
    hits = 0;
    for (int i = 0; i < 8; i++) hits += pattern[i];
    CHECK(hits == 3, "euclidean: 3 hits in 8 steps");

    /* ---- MIDI Probability ---- */
    printf("\n--- MIDI Probability ---\n");
    srand(42);
    int fire_count = 0;
    for (int i = 0; i < 1000; i++) {
        if (wb_midi_probability(0.5f)) fire_count++;
    }
    CHECK(fire_count > 400 && fire_count < 600, "probability: ~50% fire rate");

    /* ---- MIDI Ratchet ---- */
    printf("\n--- MIDI Ratchet ---\n");
    int vels[8];
    int n_vels = wb_midi_ratchet(60, 100, 4, vels);
    CHECK(n_vels == 4, "ratchet: 4 velocities");
    CHECK(vels[0] == 100, "ratchet: first vel = 100");
    CHECK(vels[1] == 80, "ratchet: second vel = 80 (decay)");
    CHECK(vels[2] < vels[1], "ratchet: decay continues");

    /* ---- MIDI Aftertouch ---- */
    printf("\n--- MIDI Aftertouch ---\n");
    wb_midi_aftertouch at;
    wb_midi_aftertouch_init(&at);
    wb_midi_aftertouch_set_channel(&at, 64);
    float mod = wb_midi_aftertouch_mod(&at, 60);
    CHECK(fabsf(mod - 64.0f/127.0f) < 0.05f, "aftertouch: channel pressure");

    wb_midi_aftertouch_set_poly(&at, 60, 127);
    mod = wb_midi_aftertouch_mod(&at, 60);
    CHECK(fabsf(mod - 1.0f) < 0.05f, "aftertouch: poly pressure overrides");

    /* ---- Automation Track ---- */
    printf("\n--- Automation Track ---\n");
    wb_automation_track track;
    wb_automation_init(&track, 16);
    wb_automation_set_mode(&track, WB_AUTOMATION_WRITE);
    wb_automation_add_keyframe(&track, 0.0f, 0.0f);
    wb_automation_add_keyframe(&track, 1.0f, 1.0f);
    wb_automation_add_keyframe(&track, 2.0f, 0.5f);

    float val = wb_automation_eval(&track, 0.0f, WB_KF_LINEAR);
    CHECK(fabsf(val - 0.0f) < 0.01f, "automation: t=0 → 0");
    val = wb_automation_eval(&track, 1.0f, WB_KF_LINEAR);
    CHECK(fabsf(val - 1.0f) < 0.01f, "automation: t=1 → 1");
    val = wb_automation_eval(&track, 0.5f, WB_KF_LINEAR);
    CHECK(fabsf(val - 0.5f) < 0.01f, "automation: t=0.5 → 0.5 (linear)");

    val = wb_automation_eval(&track, 0.5f, WB_KF_EASE_INOUT);
    CHECK(val > 0.4f && val < 0.6f, "automation: ease_inout at midpoint");

    /* ---- Snapshots / Morph ---- */
    printf("\n--- Snapshots / Morph ---\n");
    wb_snapshot_bank bank;
    wb_snapshots_init(&bank, 8);
    float params_a[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float params_b[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    int idx_a = wb_snapshots_save(&bank, params_a, 4, "all_off");
    int idx_b = wb_snapshots_save(&bank, params_b, 4, "all_on");
    CHECK(idx_a == 0 && idx_b == 1, "snapshots: saved 2");

    float morphed[4];
    wb_snapshots_morph(&bank, idx_a, idx_b, 0.5f, morphed, 4);
    CHECK(fabsf(morphed[0] - 0.5f) < 0.1f, "morph: midpoint ≈ 0.5");

    wb_snapshots_morph(&bank, idx_a, idx_b, 0.0f, morphed, 4);
    CHECK(fabsf(morphed[0] - 0.0f) < 0.01f, "morph: t=0 → snapshot A");

    wb_snapshots_morph(&bank, idx_a, idx_b, 1.0f, morphed, 4);
    CHECK(fabsf(morphed[0] - 1.0f) < 0.01f, "morph: t=1 → snapshot B");

    /* ---- NULL safety ---- */
    printf("\n--- NULL Safety ---\n");
    wb_cookie_cutter(NULL, w, h, WB_MASK_CIRCLE, 0.5f, 0.5f, 0.3f);
    wb_mirror_quad(NULL, NULL, w, h);
    wb_kaleidoscope(NULL, NULL, w, h, 4);
    wb_swirl(NULL, NULL, w, h, 0, 0);
    wb_spherize(NULL, NULL, w, h, 0, 0);
    wb_zoom_punch(NULL, NULL, w, h, 1);
    wb_scramble(NULL, NULL, w, h, 0, 8);
    wb_crt_effect(NULL, NULL, w, h, 0, 0);
    CHECK(1, "NULL inputs don't crash");

    /* Summary */
    printf("\n=== Results: %d/%d passed ===\n", passed, passed + failed);

    free(img);
    free(out);
    free(stutter_out);
    free(small_img);

    return failed > 0 ? 1 : 0;
}
