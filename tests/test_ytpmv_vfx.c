/* test_ytpmv_vfx.c — Tests for YTPMV VFX Engine (R128) */
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
    
    /* Test 1: Beat track from BPM */
    vfx_beat_track bt;
    vfx_beat_track_init(&bt);
    vfx_beat_track_from_bpm(&bt, 120.0f, 2.0f);
    ASSERT(bt.n_beats == 4, "120 BPM for 2s = 4 beats");
    ASSERT(fabsf(bt.times[0]) < 0.001f, "First beat at t=0");
    ASSERT(fabsf(bt.times[1] - 0.5f) < 0.01f, "Second beat at t=0.5");
    ASSERT(fabsf(bt.times[2] - 1.0f) < 0.01f, "Third beat at t=1.0");
    ASSERT(fabsf(bt.times[3] - 1.5f) < 0.01f, "Fourth beat at t=1.5");
    
    /* Test 2: Beat track from different BPM */
    vfx_beat_track_from_bpm(&bt, 100.0f, 3.0f);
    ASSERT(bt.n_beats == 5, "100 BPM for 3s = 5 beats");
    ASSERT(fabsf(bt.times[1] - 0.6f) < 0.01f, "100 BPM second beat at t=0.6");
    
    /* Test 3: Zoom pulse generation */
    vfx_zoom_pulse_config zoom_cfg = { .amount = 0.08f, .duration = 0.05f, .base_zoom = 1.0f };
    char zoom_filter[2048];
    int len = vfx_generate_zoom_pulse(&zoom_cfg, &bt, zoom_filter, sizeof(zoom_filter));
    ASSERT(len > 0, "Zoom pulse filter generated");
    ASSERT(strstr(zoom_filter, "zoompan") != NULL, "Contains zoompan");
    ASSERT(strstr(zoom_filter, "0.08") != NULL, "Contains zoom amount 0.08");
    printf("  zoom filter: %.80s...\n", zoom_filter);
    
    /* Test 4: Shake generation */
    vfx_shake_config shake_cfg = { .intensity = 5.0f, .frequency = 20.0f, .beat_synced = 1 };
    char shake_filter[2048];
    len = vfx_generate_shake(&shake_cfg, &bt, shake_filter, sizeof(shake_filter));
    ASSERT(len > 0, "Shake filter generated");
    ASSERT(strstr(shake_filter, "translate") != NULL, "Contains translate");
    ASSERT(strstr(shake_filter, "5.0") != NULL, "Contains intensity 5.0");
    printf("  shake filter: %.80s...\n", shake_filter);
    
    /* Test 5: Flash generation */
    vfx_flash_config flash_cfg = { .intensity = 0.3f, .duration = 0.03f, .r = 255, .g = 255, .b = 255 };
    char flash_filter[4096];
    len = vfx_generate_flash(&flash_cfg, &bt, flash_filter, sizeof(flash_filter));
    ASSERT(len > 0, "Flash filter generated");
    ASSERT(strstr(flash_filter, "geq") != NULL, "Contains geq");
    ASSERT(strstr(flash_filter, "r='") != NULL, "Contains red channel");
    printf("  flash filter: %.80s...\n", flash_filter);
    
    /* Test 6: RGB shift generation */
    vfx_rgb_shift_config rgb_cfg = { .max_offset = 4.0f, .beat_synced = 0 };
    char rgb_filter[2048];
    len = vfx_generate_rgb_shift(&rgb_cfg, &bt, rgb_filter, sizeof(rgb_filter));
    ASSERT(len > 0, "RGB shift filter generated");
    ASSERT(strstr(rgb_filter, "split=3") != NULL, "Splits into 3 channels");
    ASSERT(strstr(rgb_filter, "blend") != NULL, "Blends channels back");
    printf("  rgb filter: %.80s...\n", rgb_filter);
    
    /* Test 7: Beat detection on synthetic signal */
    /* Create a 120 BPM pulse train (beat every 0.5s) */
    float audio[44100 * 2]; /* 2 seconds at 44100 Hz */
    memset(audio, 0, sizeof(audio));
    for (int beat = 0; beat < 4; beat++) {
        int start = (int)(beat * 0.5f * 44100);
        for (int i = 0; i < 2205; i++) { /* 50ms pulse */
            float env = (float)i / 2205.0f;
            audio[start + i] = 32767.0f * (1.0f - env) * ((i % 100 < 50) ? 1.0f : -1.0f);
        }
    }
    
    vfx_beat_track detected;
    vfx_beat_track_init(&detected);
    int n_detected = vfx_detect_beats(audio, 44100 * 2, 1, 44100.0f, &detected);
    printf("  Detected %d beats (expected ~4)\n", n_detected);
    ASSERT(n_detected >= 2 && n_detected <= 8, "Beat detection found reasonable number of beats");
    if (n_detected >= 2) {
        float avg_interval = (detected.times[n_detected-1] - detected.times[0]) / (n_detected - 1);
        printf("  Avg interval: %.3f s (expected ~0.5)\n", avg_interval);
        ASSERT(fabsf(avg_interval - 0.5f) < 0.15f, "Beat interval close to 0.5s");
    }
    
    /* Test 8: Empty beat track */
    vfx_beat_track empty_bt;
    vfx_beat_track_init(&empty_bt);
    char empty_filter[2048];
    len = vfx_generate_zoom_pulse(&zoom_cfg, &empty_bt, empty_filter, sizeof(empty_filter));
    ASSERT(len > 0, "Zoom pulse with empty beat track");
    ASSERT(strstr(empty_filter, "1.00") != NULL, "Default zoom 1.0 with no beats");
    
    printf("\n=== VFX Engine: %d passed, %d failed ===\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
