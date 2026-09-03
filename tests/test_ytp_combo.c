/* test_ytp_combo.c — YTP combination effects tests (R094f) */
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
    uint8_t *img, *out;
    int w = 64, h = 64, sz = w * h * 4;

    printf("=== YTP Combo Effects (R094f) ===\n\n");

    img = (uint8_t *)calloc(sz, 1);
    out = (uint8_t *)calloc(sz, 1);

    /* ---- Animated Flip ---- */
    printf("--- Animated Flip / Spin ---\n");
    for (int i = 0; i < w*h; i++) {
        img[i*4] = (uint8_t)(i % 256);
        img[i*4+3] = 255;
    }
    wb_animated_flip(out, img, w, h, 0.0f);
    CHECK(out[0] == img[0], "flip(0): no change at progress=0");
    wb_animated_flip(out, img, w, h, 1.0f);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "flip(1): center still visible");

    wb_spin(out, img, w, h, 0.0f);
    CHECK(out[0] == img[0], "spin(0): no change at 0 deg");
    wb_spin(out, img, w, h, 90.0f);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "spin(90): center visible");
    wb_spin(out, img, w, h, 180.0f);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "spin(180): center visible");

    /* ---- Paint Jobs ---- */
    printf("\n--- Paint Jobs ---\n");
    memset(img, 0, sz);
    wb_paint_line(img, w, h, 10, 10, 50, 50, 255, 0, 0, 2);
    CHECK(img[(30*w+30)*4] == 255, "paint_line: red pixel on line");

    memset(img, 0, sz);
    wb_paint_circle(img, w, h, 32, 32, 10, 0, 255, 0, 1);
    CHECK(img[(32*w+32)*4+1] == 255, "paint_circle: green center filled");

    memset(img, 0, sz);
    wb_paint_arrow(img, w, h, 10, 10, 50, 50, 0, 0, 255);
    CHECK(img[(30*w+30)*4+2] == 255, "paint_arrow: blue pixel on arrow");

    /* ---- Meme Replace ---- */
    printf("\n--- Meme Replace ---\n");
    float audio[48000] = {0}; /* 1s at 48kHz mono */
    float meme[4800] = {0};   /* 100ms meme */
    for (int i = 0; i < 4800; i++) meme[i] = 0.5f * sinf(i * 0.1f);

    int written = wb_meme_replace(audio, 48000, 1, 48000.0f, 24000, meme, 4800);
    CHECK(written == 4800, "meme_replace: wrote 4800 frames");
    /* Check that some non-zero sample was inserted (sine wave has peaks) */
    float max_val = 0;
    for (int i = 24000; i < 28800; i++)
        if (fabsf(audio[i]) > max_val) max_val = fabsf(audio[i]);
    CHECK(max_val > 0.1f, "meme_replace: audio inserted");
    /* Check fade in */
    CHECK(fabsf(audio[24000]) <= fabsf(audio[25000]) + 0.01f, "meme_replace: fade in");

    /* ---- Scramble Stutter ---- */
    printf("\n--- Scramble Stutter ---\n");
    uint8_t *stutter_out = (uint8_t *)malloc(sz * 4);
    for (int i = 0; i < w*h; i++) {
        img[i*4] = (uint8_t)(i % 256);
        img[i*4+3] = 255;
    }
    wb_scramble_stutter(stutter_out, img, w, h, 42, 8, 4);
    CHECK(stutter_out[0*4+3] == 255, "scramble_stutter: frame 0 has content");
    /* All 4 frames should be identical (stutter) */
    int identical = 1;
    for (int f = 1; f < 4; f++) {
        if (memcmp(stutter_out, stutter_out + f * sz, sz) != 0) {
            identical = 0; break;
        }
    }
    CHECK(identical, "scramble_stutter: all frames identical");

    /* ---- Source Abuse ---- */
    printf("\n--- Source Abuse ---\n");
    for (int i = 0; i < w*h; i++) {
        img[i*4] = 200; img[i*4+1] = 100; img[i*4+2] = 50; img[i*4+3] = 255;
    }
    wb_source_abuse(out, img, w, h, 0, 42);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "source_abuse: output has content");

    wb_source_abuse(out, img, w, h, 5, 42);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "source_abuse: iteration 5 has content");

    /* ---- Subversion Poop ---- */
    printf("\n--- Subversion Poop ---\n");
    for (int i = 0; i < w*h; i++) {
        img[i*4] = 128; img[i*4+1] = 128; img[i*4+2] = 128; img[i*4+3] = 255;
    }
    wb_subversion_poop(out, img, w, h, 0.0f, 42);
    CHECK(out[0*4] == 128, "subversion(0): no change at chaos=0");

    wb_subversion_poop(out, img, w, h, 0.5f, 42);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "subversion(0.5): output has content");

    wb_subversion_poop(out, img, w, h, 1.0f, 42);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "subversion(1.0): full chaos output");

    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    wb_animated_flip(NULL, NULL, w, h, 0);
    wb_spin(NULL, NULL, w, h, 0);
    wb_paint_line(NULL, w, h, 0, 0, 0, 0, 0, 0, 0, 0);
    wb_paint_circle(NULL, w, h, 0, 0, 0, 0, 0, 0, 0);
    wb_paint_arrow(NULL, w, h, 0, 0, 0, 0, 0, 0, 0);
    wb_meme_replace(NULL, 0, 0, 0, 0, NULL, 0);
    wb_scramble_stutter(NULL, NULL, w, h, 0, 0, 0);
    wb_source_abuse(NULL, NULL, w, h, 0, 0);
    wb_subversion_poop(NULL, NULL, w, h, 0, 0);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", p, p + f);

    free(img);
    free(out);
    free(stutter_out);

    return f > 0 ? 1 : 0;
}
