/* test_ytp_remaining.c — Final YTP gap closers (R094g) */
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

    printf("=== YTP Remaining (R094g) ===\n\n");
    img = (uint8_t *)calloc(sz, 1);
    out = (uint8_t *)calloc(sz, 1);

    /* ---- Sex-O-Phone ---- */
    printf("--- Sex-O-Phone ---\n");
    float *audio = (float *)calloc(4800, sizeof(float));
    wb_sexophone_gen(audio, 4800, 48000.0f, 220.0f, 0.8f);
    float max_val = 0;
    for (int i = 0; i < 4800; i++)
        if (fabsf(audio[i]) > max_val) max_val = fabsf(audio[i]);
    CHECK(max_val > 0.01f, "sexophone: generates audio");
    CHECK(max_val <= 1.0f, "sexophone: no clipping");

    for (int i = 0; i < w*h; i++) {
        img[i*4] = 200; img[i*4+1] = 150; img[i*4+2] = 100; img[i*4+3] = 255;
    }
    wb_sexophone_visual(out, img, w, h, 0.0f);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "sexophone_visual: output has content");
    wb_sexophone_visual(out, img, w, h, 0.5f);
    CHECK(out[(h/2*w+w/2)*4+3] == 255, "sexophone_visual: phase 0.5 has content");
    free(audio);

    /* ---- Dance Rave ---- */
    printf("\n--- Dance Rave ---\n");
    for (int i = 0; i < w*h; i++) {
        img[i*4] = 200; img[i*4+1] = 100; img[i*4+2] = 50; img[i*4+3] = 255;
    }
    wb_dance_rave(out, img, w, h, 0.0f, 0);
    CHECK(out[0*4+3] == 255, "rave(0): no strobe output");
    wb_dance_rave(out, img, w, h, 0.0f, 1);
    CHECK(out[0*4] > img[0*4], "rave: strobe boosts red");
    wb_dance_rave(out, img, w, h, 0.25f, 0);
    CHECK(out[0*4+3] == 255, "rave: color cycle output");

    /* ---- Tennis Rally ---- */
    printf("\n--- Tennis Rally ---\n");
    float *tennis = (float *)calloc(100 * 2, sizeof(float));
    for (int i = 0; i < 100; i++) {
        tennis[i*2] = (float)i;
        tennis[i*2+1] = (float)(i + 1000);
    }
    wb_tennis_rally(tennis, 100, 2, 10, 20, 50, 60);
    /* After swap, position 10 should have what was at 50 */
    CHECK(tennis[10*2] == 50.0f, "tennis: segment A got B's data");
    CHECK(tennis[50*2] == 10.0f, "tennis: segment B got A's data");
    free(tennis);

    /* ---- Scramble Permutation ---- */
    printf("\n--- Scramble Perm ---\n");
    int perm[16];
    wb_scramble_perm(perm, 16, 42);
    int all_present = 1;
    for (int v = 0; v < 16; v++) {
        int found = 0;
        for (int i = 0; i < 16; i++)
            if (perm[i] == v) { found = 1; break; }
        if (!found) { all_present = 0; break; }
    }
    CHECK(all_present, "scramble_perm: all values present (valid permutation)");

    /* Different seed → different permutation */
    int perm2[16];
    wb_scramble_perm(perm2, 16, 99);
    int different = 0;
    for (int i = 0; i < 16; i++)
        if (perm[i] != perm2[i]) { different = 1; break; }
    CHECK(different, "scramble_perm: different seed → different perm");

    /* ---- Stutter Iter FX ---- */
    printf("\n--- Stutter Iter FX ---\n");
    for (int i = 0; i < w*h; i++) {
        img[i*4] = 200; img[i*4+1] = 100; img[i*4+2] = 50; img[i*4+3] = 255;
    }
    /* Effect 0 = normal */
    wb_stutter_iter_fx(img, w, h, 0);
    CHECK(img[0*4] == 200, "stutter_iter(0): normal");

    /* Effect 1 = invert */
    wb_stutter_iter_fx(img, w, h, 1);
    CHECK(img[0*4] == 55, "stutter_iter(1): inverted (255-200=55)");

    /* Reset and test R-only */
    for (int i = 0; i < w*h; i++) {
        img[i*4] = 200; img[i*4+1] = 100; img[i*4+2] = 50; img[i*4+3] = 255;
    }
    wb_stutter_iter_fx(img, w, h, 2);
    CHECK(img[0*4+1] == 0 && img[0*4+2] == 0, "stutter_iter(2): R-only");

    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    wb_sexophone_gen(NULL, 0, 0, 0, 0);
    wb_sexophone_visual(NULL, NULL, w, h, 0);
    wb_dance_rave(NULL, NULL, w, h, 0, 0);
    wb_tennis_rally(NULL, 0, 0, 0, 0, 0, 0);
    wb_scramble_perm(NULL, 0, 0);
    wb_stutter_iter_fx(NULL, w, h, 0);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", p, p + f);

    free(img);
    free(out);
    return f > 0 ? 1 : 0;
}
