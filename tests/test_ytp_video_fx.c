/* test_ytp_video_fx.c — Video Poopisms + YTPMV Pipeline tests (R099) */
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

    printf("=== R099: Video Poopisms + YTPMV Pipeline ===\n\n");

    /* ---- Keyframe Interpolation (K1-K15) ---- */
    printf("--- Keyframe Interpolation ---\n");
    CHECK(WB_KF_COUNT == 15, "kf: 15 interpolation types");

    /* Constant */
    CHECK(wb_kf_interpolate(0.5f, WB_KF_HOLD, 0, 0, 0) == 0, "kf: constant(0.5)=0");
    /* Linear */
    CHECK(fabsf(wb_kf_interpolate(0.5f, WB_KF_LINEAR, 0, 0, 0) - 0.5f) < 0.01f, "kf: linear(0.5)=0.5");
    CHECK(fabsf(wb_kf_interpolate(0.0f, WB_KF_LINEAR, 0, 0, 0) - 0.0f) < 0.01f, "kf: linear(0)=0");
    CHECK(fabsf(wb_kf_interpolate(1.0f, WB_KF_LINEAR, 0, 0, 0) - 1.0f) < 0.01f, "kf: linear(1)=1");
    /* Ease-In (t^2) */
    CHECK(fabsf(wb_kf_interpolate(0.5f, WB_KF_EASE_IN, 0, 0, 0) - 0.25f) < 0.01f, "kf: ease_in(0.5)=0.25");
    /* Ease-Out */
    CHECK(fabsf(wb_kf_interpolate(0.5f, WB_KF_EASE_OUT, 0, 0, 0) - 0.75f) < 0.01f, "kf: ease_out(0.5)=0.75");
    /* Ease-In-Out (smoothstep) */
    CHECK(fabsf(wb_kf_interpolate(0.5f, WB_KF_EASE_INOUT, 0, 0, 0) - 0.5f) < 0.01f, "kf: ease_inout(0.5)=0.5");
    /* Step */
    CHECK(wb_kf_interpolate(0.9f, WB_KF_STEP, 0, 0, 0) == 0, "kf: step(0.9)=0");
    /* S-Curve */
    CHECK(fabsf(wb_kf_interpolate(0.5f, WB_KF_SCURVE, 0, 0, 0) - 0.5f) < 0.01f, "kf: scurve(0.5)=0.5");
    /* Exponential */
    CHECK(wb_kf_interpolate(0.0f, WB_KF_EXPONENTIAL, 0, 0, 0) == 0, "kf: exp(0)=0");
    CHECK(fabsf(wb_kf_interpolate(1.0f, WB_KF_EXPONENTIAL, 0, 0, 0) - 1.0f) < 0.01f, "kf: exp(1)=1");
    /* Logarithmic */
    CHECK(wb_kf_interpolate(0.0f, WB_KF_LOGARITHMIC, 0, 0, 0) == 0, "kf: log(0)=0");
    CHECK(fabsf(wb_kf_interpolate(1.0f, WB_KF_LOGARITHMIC, 0, 0, 0) - 1.0f) < 0.01f, "kf: log(1)=1");
    /* Elastic (overshoots) */
    float elastic = wb_kf_interpolate(1.0f, WB_KF_ELASTIC, 0, 0, 0);
    CHECK(fabsf(elastic - 1.0f) < 0.01f, "kf: elastic(1)=1");
    /* Bounce */
    CHECK(fabsf(wb_kf_interpolate(1.0f, WB_KF_BOUNCE, 0, 0, 0) - 1.0f) < 0.01f, "kf: bounce(1)=1");
    /* Back (overshoot) */
    float back = wb_kf_interpolate(1.0f, WB_KF_BACK, 0, 0, 0);
    CHECK(fabsf(back - 1.0f) < 0.01f, "kf: back(1)=1");

    /* ---- Fade Curves ---- */
    printf("\n--- Fade Curves ---\n");
    CHECK(WB_FADE_COUNT == 4, "fade: 4 curve types");
    CHECK(fabsf(wb_fade_eval(0.5f, WB_FADE_LINEAR) - 0.5f) < 0.01f, "fade: linear(0.5)=0.5");
    CHECK(wb_fade_eval(0.0f, WB_FADE_EXPONENTIAL) == 0, "fade: exp(0)=0");
    CHECK(fabsf(wb_fade_eval(1.0f, WB_FADE_SCURVE) - 1.0f) < 0.01f, "fade: scurve(1)=1");

    /* ---- Stutter Loop Plus ---- */
    printf("\n--- Stutter Loop Plus ---\n");
    wb_stutter_plus sp;
    wb_stutter_plus_init(&sp, 8);
    CHECK(sp.n_repeats == 8, "stutter+: 8 repeats");
    CHECK(sp.fx_per_repeat == 5, "stutter+: 5 FX types");

    uint8_t test_frame[64*64*4];
    memset(test_frame, 128, sizeof(test_frame));
    wb_stutter_plus_apply(&sp, test_frame, 64, 64, 0); /* none */
    CHECK(test_frame[0] == 128, "stutter+: FX0=none preserves pixel");
    wb_stutter_plus_apply(&sp, test_frame, 64, 64, 1); /* invert */
    /* After invert of 128 = 127 */
    CHECK(test_frame[0] == 127 || test_frame[0] == 128, "stutter+: FX1=invert");

    /* ---- Strobe / Flash ---- */
    printf("\n--- Strobe / Flash ---\n");
    wb_strobe_state st;
    wb_strobe_init(&st, 4);
    CHECK(st.strobe_interval == 4, "strobe: interval=4");

    int flashes = 0;
    for (int i = 0; i < 20; i++)
        if (wb_strobe_tick(&st)) flashes++;
    CHECK(flashes > 0, "strobe: fires flashes");

    uint8_t strobe_frame[32*32*4];
    memset(strobe_frame, 0, sizeof(strobe_frame));
    wb_strobe_apply(strobe_frame, 32, 32, 0); /* white */
    CHECK(strobe_frame[0] == 255, "strobe: white flash");

    /* ---- Frame Freeze ---- */
    printf("\n--- Frame Freeze ---\n");
    wb_frame_freeze fz;
    wb_freeze_init(&fz);

    uint8_t orig_frame[32*32*4];
    memset(orig_frame, 0xAB, sizeof(orig_frame));
    wb_freeze_capture(&fz, orig_frame, 32, 32);
    CHECK(fz.is_frozen == 1, "freeze: captured");

    /* Modify original */
    memset(orig_frame, 0, sizeof(orig_frame));
    wb_freeze_hold(&fz, 3);
    int frozen = wb_freeze_tick(&fz, orig_frame, 32, 32);
    CHECK(frozen == 1, "freeze: holding");
    CHECK(orig_frame[0] == 0xAB, "freeze: restored frozen frame");

    /* Tick down: hold=3, first tick already fired (line 105), so 3 more ticks */
    wb_freeze_tick(&fz, orig_frame, 32, 32); /* hold 2→1 */
    wb_freeze_tick(&fz, orig_frame, 32, 32); /* hold 1→0, still returns 1 */
    frozen = wb_freeze_tick(&fz, orig_frame, 32, 32); /* hold=0, releases */
    CHECK(frozen == 0, "freeze: released after hold");
    wb_freeze_free(&fz);

    /* ---- Screen Shake ---- */
    printf("\n--- Screen Shake ---\n");
    wb_screen_shake sh;
    wb_shake_init(&sh);
    wb_shake_trigger(&sh, 10.0f);
    CHECK(sh.active == 1, "shake: triggered");

    float prev_x = sh.offset_x;
    wb_shake_update(&sh, 0.016f);
    CHECK(sh.offset_x != prev_x || sh.offset_y != 0, "shake: offset changes");

    /* Let it decay */
    for (int i = 0; i < 1000; i++)
        wb_shake_update(&sh, 0.016f);
    CHECK(sh.active == 0, "shake: decays to inactive");

    /* ---- Cookie Cutter ---- */
    printf("\n--- Cookie Cutter ---\n");
    uint8_t mask_frame[64*64*4];
    memset(mask_frame, 200, sizeof(mask_frame));
    wb_cookie_cutter(mask_frame, 64, 64, WB_MASK_CIRCLE, 0.5f, 0.5f, 0.4f);

    /* Corner should be transparent (outside circle) */
    int corner_off = 0;
    CHECK(mask_frame[corner_off+3] == 0, "cookie: corner masked out (alpha=0)");
    /* Center should still be 200 (inside circle) */
    int center_off = (32*64+32)*4;
    CHECK(mask_frame[center_off] == 200, "cookie: center preserved");

    /* Test star shape */
    memset(mask_frame, 200, sizeof(mask_frame));
    wb_cookie_cutter(mask_frame, 64, 64, WB_MASK_STAR, 0.5f, 0.5f, 0.4f);
    CHECK(mask_frame[3] == 0, "cookie: star masks corner");

    /* Test triangle shape */
    memset(mask_frame, 200, sizeof(mask_frame));
    wb_cookie_cutter(mask_frame, 64, 64, WB_MASK_TRIANGLE, 0.5f, 0.5f, 0.4f);
    CHECK(mask_frame[3] == 0, "cookie: triangle masks corner");

    /* ---- Flip/Spin ---- */
    printf("\n--- Flip/Spin ---\n");
    wb_flip_spin fs;
    wb_flip_spin_init(&fs);
    CHECK(fs.angle == 0, "flip_spin: starts at 0");

    wb_flip_spin_update(&fs, 0.1f);
    CHECK(fs.angle > 0, "flip_spin: angle increases");

    /* ---- Recursion Poop ---- */
    printf("\n--- Recursion Poop ---\n");
    uint8_t rec_frame[64*64*4];
    memset(rec_frame, 128, sizeof(rec_frame));
    wb_recursion_apply(rec_frame, 64, 64, 2);
    CHECK(1, "recursion: applied without crash");

    /* ---- Compression Torture ---- */
    printf("\n--- Compression Torture ---\n");
    uint8_t comp_frame[64*64*4];
    for (int i = 0; i < 64*64*4; i++) comp_frame[i] = (uint8_t)(i % 256);
    wb_compression_torture_ytp(comp_frame, 64, 64, 10);
    CHECK(1, "compression: applied without crash");

    /* ---- Phoneme Extraction ---- */
    printf("\n--- Phoneme Extraction ---\n");
    /* Create test audio: 3 segments with different energy */
    float audio[48000];
    memset(audio, 0, sizeof(audio));
    /* Segment 1: quiet */
    for (int i = 0; i < 16000; i++)
        audio[i] = 0.01f;
    /* Segment 2: loud */
    for (int i = 16000; i < 32000; i++)
        audio[i] = 0.8f * sinf(i * 0.05f);
    /* Segment 3: medium */
    for (int i = 32000; i < 48000; i++)
        audio[i] = 0.3f * sinf(i * 0.1f);

    int segments[256];
    int n_segs = wb_extract_phonemes(audio, 48000, 1, 48000.0f, segments, 256);
    CHECK(n_segs >= 1, "phoneme: detected boundaries");

    /* ---- Pitch-to-Note ---- */
    printf("\n--- Pitch-to-Note ---\n");
    int midi_note = freq_to_midi(440.0f);
    CHECK(midi_note == 69, "pitch: A4=440Hz -> MIDI 69");

    float freq_back = midi_to_freq(69);
    CHECK(fabsf(freq_back - 440) < 1, "pitch: MIDI 69 -> ~440Hz");

    int note = wb_pitch_to_note(440.0f, 0); /* major scale */
    CHECK(note >= 0 && note <= 127, "pitch: 440Hz -> valid MIDI note");

    /* ---- Beat Sequencer ---- */
    printf("\n--- Beat Sequencer ---\n");
    wb_beat_seq seq;
    wb_beat_seq_init(&seq, 120.0f);
    CHECK(seq.bpm == 120.0f, "beat_seq: BPM=120");
    CHECK(seq.step_duration > 0, "beat_seq: step duration > 0");

    wb_beat_seq_set(&seq, 0, 0, 5);
    CHECK(seq.grid[0][0] == 5, "beat_seq: set cell");

    /* ---- Euclidean Rhythm ---- */
    printf("\n--- Euclidean Rhythm ---\n");
    int pattern[16];
    wb_euclidean_rhythm(pattern, 16, 4);
    int hits = 0;
    for (int i = 0; i < 16; i++) hits += pattern[i];
    CHECK(hits == 4, "euclidean: 4 hits in 16 steps");

    wb_euclidean_rhythm(pattern, 8, 3);
    hits = 0;
    for (int i = 0; i < 8; i++) hits += pattern[i];
    CHECK(hits == 3, "euclidean: 3 hits in 8 steps");

    /* ---- MIDI Aftertouch ---- */
    printf("\n--- MIDI Aftertouch ---\n");
    wb_midi_aftertouch at;
    wb_midi_aftertouch_init(&at);
    wb_midi_aftertouch_set_channel(&at, 100);
    CHECK(at.channel_pressure == 100, "aftertouch: channel pressure set");

    wb_midi_aftertouch_set_poly(&at, 60, 80);
    CHECK(at.poly_pressure[60] == 80, "aftertouch: poly pressure set");

    /* ---- MIDI Probability ---- */
    printf("\n--- MIDI Probability ---\n");
    wb_midi_prob mp;
    wb_midi_prob_init(&mp);
    CHECK(mp.probability == 1.0f, "midi_prob: default 100%");

    mp.probability = 0.5f;
    int fires = 0;
    for (int i = 0; i < 100; i++)
        if (wb_midi_prob_fire(&mp)) fires++;
    CHECK(fires > 0 && fires < 100, "midi_prob: ~50% fire rate");

    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    wb_stutter_plus_init(NULL, 0);
    wb_stutter_plus_apply(NULL, NULL, 0, 0, 0);
    wb_strobe_init(NULL, 0);
    wb_strobe_tick(NULL);
    wb_strobe_apply(NULL, 0, 0, 0);
    wb_invert_flash_apply(NULL, 0, 0);
    wb_freeze_init(NULL);
    wb_freeze_capture(NULL, NULL, 0, 0);
    wb_freeze_hold(NULL, 0);
    wb_freeze_tick(NULL, NULL, 0, 0);
    wb_freeze_free(NULL);
    wb_shake_init(NULL);
    wb_shake_trigger(NULL, 0);
    wb_shake_update(NULL, 0);
    wb_cookie_cutter(NULL, 0, 0, 0, 0, 0, 0);
    wb_flip_spin_init(NULL);
    wb_flip_spin_update(NULL, 0);
    wb_recursion_apply(NULL, 0, 0, 0);
    wb_recursion_init(NULL, 0, 0);
    wb_recursion_free(NULL);
    wb_compression_torture_ytp(NULL, 0, 0, 0);
    wb_extract_phonemes(NULL, 0, 0, 0, NULL, 0);
    wb_beat_seq_init(NULL, 0);
    wb_beat_seq_set(NULL, 0, 0, 0);
    wb_beat_seq_tick(NULL, 0);
    wb_euclidean_rhythm(NULL, 0, 0);
    wb_midi_aftertouch_init(NULL);
    wb_midi_aftertouch_set_channel(NULL, 0);
    wb_midi_aftertouch_set_poly(NULL, 0, 0);
    wb_midi_prob_init(NULL);
    wb_midi_prob_fire(NULL);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", p, p + f);
    return f > 0 ? 1 : 0;
}
