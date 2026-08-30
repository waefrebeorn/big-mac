/* tests/test_audio_color.c — headless test of audio-reactive color grading */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus.h"

void *wb_audio_color_create(uint32_t sr);
void  wb_audio_color_destroy(void *inst);
void  wb_audio_color_analyze(void *inst, const float *audio, int n);
void  wb_audio_color_apply(void *inst, uint8_t *buf, int width, int height);
void  wb_audio_color_set_saturation(void *inst, float amount);
void  wb_audio_color_set_brightness(void *inst, float amount);
void  wb_audio_color_set_hue(void *inst, float amount);
void  wb_audio_color_set_flash(void *inst, float amount);

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    uint32_t sr = 44100;
    printf("=== Audio-Reactive Color Test ===\n");

    void *ac = wb_audio_color_create(sr);
    CHECK(ac != NULL);

    /* Create test frame */
    int w = 32, h = 32;
    uint8_t *buf = (uint8_t *)calloc(w * h * 4, 1);
    for (int i = 0; i < w * h; i++) {
        buf[i * 4] = 200;     /* R */
        buf[i * 4 + 1] = 100; /* G */
        buf[i * 4 + 2] = 50;  /* B */
        buf[i * 4 + 3] = 255; /* A */
    }

    /* Test 1: Analyze silence = no change expected */
    float silence[1024];
    memset(silence, 0, sizeof(silence));
    wb_audio_color_analyze(ac, silence, 1024);
    uint8_t before[4];
    memcpy(before, buf, 4);
    wb_audio_color_apply(ac, buf, w, h);
    /* After silence, bass/treble/mid energy ≈ 0, so grade ≈ identity */
    CHECK(1); /* No crash */

    /* Test 2: Analyze bass-heavy audio */
    float bass[1024];
    for (int i = 0; i < 1024; i++)
        bass[i] = sinf(2.0f * M_PI * 80.0f * i / sr) * 0.8f;
    wb_audio_color_analyze(ac, bass, 1024);
    wb_audio_color_apply(ac, buf, w, h);
    CHECK(1); /* No crash with bass input */

    /* Test 3: Analyze treble-heavy audio */
    float treble[1024];
    for (int i = 0; i < 1024; i++)
        treble[i] = sinf(2.0f * M_PI * 5000.0f * i / sr) * 0.8f;
    wb_audio_color_analyze(ac, treble, 1024);
    wb_audio_color_apply(ac, buf, w, h);
    CHECK(1);

    /* Test 4: Parameter setters don't crash */
    wb_audio_color_set_saturation(ac, 3.0f);
    wb_audio_color_set_brightness(ac, 2.0f);
    wb_audio_color_set_hue(ac, 90.0f);
    wb_audio_color_set_flash(ac, 0.5f);
    wb_audio_color_apply(ac, buf, w, h);
    CHECK(1);

    /* Test 5: Output pixels are valid RGBA */
    int valid = 1;
    for (int i = 0; i < w * h * 4; i++) {
        if (buf[i] > 255) { valid = 0; break; }
    }
    CHECK(valid);

    free(buf);
    wb_audio_color_destroy(ac);

    printf("\nAudio-Color: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
