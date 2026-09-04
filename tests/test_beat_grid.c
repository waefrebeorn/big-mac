/* test_beat_grid.c — Beat Grid Quantizer tests (R109) */
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
    
    fprintf(stderr, "=== R109: Beat Grid Quantizer ===\n");
    
    /* --- Basic beat grid --- */
    fprintf(stderr, "\n-- Beat Grid Init --\n");
    
    wb_beat_grid bg;
    wb_beat_grid_init(&bg, 120.0f, 44100.0f, 4.0f);
    CHECK(bg.bpm == 120.0f, "grid: bpm set");
    CHECK(bg.sample_rate == 44100.0f, "grid: sample rate set");
    CHECK(bg.n_beats > 0, "grid: beats computed");
    CHECK(bg.n_beats == 8, "grid: 8 beats in 4s at 120bpm");
    CHECK(bg.n_subdivs > 0, "grid: subdivisions computed");
    
    /* Beat positions should be at 0, 22050, 44100, 66150, ... */
    CHECK(bg.beat_samples[0] == 0, "grid: first beat at 0");
    CHECK(abs(bg.beat_samples[1] - 22050) <= 1, "grid: second beat at 22050");
    CHECK(abs(bg.beat_samples[2] - 44100) <= 1, "grid: third beat at 44100");
    
    fprintf(stderr, "  Beats: %d, Subdivs: %d\n", bg.n_beats, bg.n_subdivs);
    
    /* --- Quantization --- */
    fprintf(stderr, "\n-- Quantization --\n");
    
    /* Quantize a position halfway between beats → should snap to nearest */
    int q = wb_beat_grid_quantize(&bg, 11025); /* halfway between 0 and 22050 */
    CHECK(q >= 0, "grid: quantize returns valid position");
    
    /* Quantize exactly on a beat → should stay */
    q = wb_beat_grid_quantize(&bg, 22050);
    CHECK(q == 22050, "grid: quantize on-beat stays put");
    
    /* Quantize near a beat → should snap */
    q = wb_beat_grid_quantize(&bg, 22500);
    CHECK(abs(q - 22050) < 500, "grid: quantize near-beat snaps");
    
    /* Quantize in seconds */
    float qs = wb_beat_grid_quantize_sec(&bg, 0.5f); /* exactly on beat 1 */
    CHECK(fabsf(qs - 0.5f) < 0.001f, "grid: quantize_sec on beat");
    
    qs = wb_beat_grid_quantize_sec(&bg, 0.51f); /* slightly off beat */
    CHECK(fabsf(qs - 0.5f) < 0.05f, "grid: quantize_sec snaps to beat");
    
    /* --- Beat detection --- */
    fprintf(stderr, "\n-- Beat Detection --\n");
    
    CHECK(wb_beat_grid_is_on_beat(&bg, 0.0f, 0.01f), "grid: t=0 is on beat");
    CHECK(wb_beat_grid_is_on_beat(&bg, 0.5f, 0.01f), "grid: t=0.5 is on beat");
    CHECK(!wb_beat_grid_is_on_beat(&bg, 0.25f, 0.001f), "grid: t=0.25 is off beat");
    
    int beat_num = wb_beat_grid_beat_at(&bg, 0.0f);
    CHECK(beat_num == 0, "grid: beat 0 at t=0");
    beat_num = wb_beat_grid_beat_at(&bg, 0.5f);
    CHECK(beat_num == 1, "grid: beat 1 at t=0.5");
    
    /* --- Phoneme quantization --- */
    fprintf(stderr, "\n-- Phoneme Quantization --\n");
    
    float starts[] = {0.01f, 0.52f, 1.03f, 1.48f};
    float durs[] = {0.24f, 0.26f, 0.23f, 0.27f};
    int n = 4;
    
    wb_beat_grid_quantize_phonemes(&bg, starts, durs, n);
    
    /* After quantization, starts should be closer to grid positions */
    CHECK(fabsf(starts[0]) < 0.05f, "grid: phoneme 0 snapped near 0");
    CHECK(fabsf(starts[1] - 0.5f) < 0.05f, "grid: phoneme 1 snapped to 0.5");
    CHECK(fabsf(starts[2] - 1.0f) < 0.05f, "grid: phoneme 2 snapped to 1.0");
    CHECK(fabsf(starts[3] - 1.5f) < 0.05f, "grid: phoneme 3 snapped to 1.5");
    
    /* Durations should be quantized to subdivisions */
    for (int i = 0; i < n; i++) {
        float expected_dur = 60.0f / bg.bpm / bg.quantize_division;
        CHECK(fabsf(durs[i] - expected_dur) < 0.01f || fabsf(durs[i] - 2*expected_dur) < 0.01f,
              "grid: phoneme duration quantized");
    }
    
    /* --- Pattern generation --- */
    fprintf(stderr, "\n-- Pattern Generation --\n");
    
    int pattern[16];
    
    /* Four on the floor */
    int hits = wb_beat_grid_generate_pattern(&bg, 0, pattern, 16);
    CHECK(hits == 4, "grid: four-on-floor = 4 hits");
    CHECK(pattern[0] == 1 && pattern[4] == 1 && pattern[8] == 1 && pattern[12] == 1,
          "grid: four-on-floor positions");
    
    /* Offbeat 8ths */
    hits = wb_beat_grid_generate_pattern(&bg, 1, pattern, 16);
    CHECK(hits == 8, "grid: offbeat-8ths = 8 hits");
    CHECK(pattern[0] == 1 && pattern[2] == 1 && pattern[4] == 1,
          "grid: offbeat-8ths positions");
    
    /* Syncopated */
    hits = wb_beat_grid_generate_pattern(&bg, 2, pattern, 16);
    CHECK(hits > 0 && hits < 16, "grid: syncopated has some hits");
    
    /* Euclidean */
    hits = wb_beat_grid_generate_pattern(&bg, 3, pattern, 16);
    CHECK(hits > 0 && hits < 16, "grid: euclidean has some hits");
    
    /* Every beat */
    hits = wb_beat_grid_generate_pattern(&bg, 4, pattern, 16);
    CHECK(hits == 16, "grid: every-beat = 16 hits");
    
    /* NULL safety */
    wb_beat_grid_init(NULL, 120, 44100, 4.0f);
    wb_beat_grid_quantize(NULL, 100);
    wb_beat_grid_quantize_sec(NULL, 1.0f);
    wb_beat_grid_quantize_phonemes(NULL, starts, durs, n);
    wb_beat_grid_quantize_phonemes(&bg, NULL, durs, n);
    wb_beat_grid_quantize_phonemes(&bg, starts, NULL, n);
    wb_beat_grid_is_on_beat(NULL, 1.0f, 0.01f);
    wb_beat_grid_beat_at(NULL, 1.0f);
    wb_beat_grid_generate_pattern(NULL, 0, pattern, 16);
    wb_beat_grid_generate_pattern(&bg, 0, NULL, 16);
    CHECK(1, "grid: NULL safety");
    
    /* --- Different BPM --- */
    fprintf(stderr, "\n-- Different BPM --\n");
    
    wb_beat_grid bg2;
    wb_beat_grid_init(&bg2, 140.0f, 44100.0f, 4.0f);
    CHECK(bg2.n_beats > bg.n_beats, "grid: higher BPM = more beats");
    
    wb_beat_grid bg3;
    wb_beat_grid_init(&bg3, 60.0f, 44100.0f, 4.0f);
    CHECK(bg3.n_beats == 4, "grid: 60bpm = 4 beats in 4s");
    
    /* --- Summary --- */
    fprintf(stderr, "\n=== R109 Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
