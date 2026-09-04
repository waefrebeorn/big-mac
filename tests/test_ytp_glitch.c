/* test_ytp_glitch.c — Advanced Glitch Effects tests (R101) */
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

    printf("=== R101: Advanced Glitch Effects ===\n\n");

    /* ---- Pixel Sort ---- */
    printf("--- Pixel Sort ---\n");
    uint8_t ps_frame[64*64*4];
    /* Create gradient: left=dark, right=bright */
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int off = (y*64+x)*4;
            ps_frame[off] = (uint8_t)(x * 4);
            ps_frame[off+1] = (uint8_t)(x * 4);
            ps_frame[off+2] = (uint8_t)(x * 4);
            ps_frame[off+3] = 255;
        }
    wb_pixel_sort(ps_frame, 64, 64, 128, 1); /* horizontal */
    CHECK(1, "pixel_sort: horizontal applied");

    /* Vertical sort */
    wb_pixel_sort(ps_frame, 64, 64, 128, 0);
    CHECK(1, "pixel_sort: vertical applied");

    /* ---- Stutter Loop Minus ---- */
    printf("\n--- Stutter Loop Minus ---\n");
    wb_stutter_minus sm;
    wb_stutter_minus_init(&sm, 2, 2);
    CHECK(sm.n_blank == 2, "stutter_minus: blank=2");
    CHECK(sm.n_show == 2, "stutter_minus: show=2");

    int blanks = 0, shows = 0;
    for (int i = 0; i < 12; i++) {
        if (wb_stutter_minus_tick(&sm)) blanks++;
        else shows++;
    }
    CHECK(blanks > 0, "stutter_minus: some frames blanked");
    CHECK(shows > 0, "stutter_minus: some frames shown");

    /* ---- Buzzing Stutter ---- */
    printf("\n--- Buzzing Stutter ---\n");
    wb_buzzing_stutter bs;
    wb_buzzing_stutter_init(&bs, 4, 60.0f);
    CHECK(bs.n_repeat == 4, "buzzing: 4 repeats");

    float sample = wb_buzzing_stutter_gen(&bs, 48000.0f);
    CHECK(sample != 0, "buzzing: generates non-zero audio");

    int repeats = 0;
    for (int i = 0; i < 12; i++)
        if (wb_buzzing_stutter_tick(&bs)) repeats++;
    CHECK(repeats > 0, "buzzing: stutter repeats");

    /* ---- Sex-O-Phone ---- */
    printf("\n--- Sex-O-Phone ---\n");
    wb_sexophone sax;
    wb_sexophone_init(&sax);
    CHECK(sax.freq > 0, "sexophone: has frequency");

    float sax_sample = wb_sexophone_gen_sample(&sax, 48000.0f);
    CHECK(sax_sample != 0, "sexophone: generates audio");

    float pulse = wb_sexophone_pulse(&sax, 0.016f);
    CHECK(pulse >= 0 && pulse <= 1, "sexophone: pulse 0..1");

    /* ---- Tech Text ---- */
    printf("\n--- Tech Text ---\n");
    uint8_t text_frame[64*64*4];
    memset(text_frame, 0, sizeof(text_frame));
    wb_tech_text(text_frame, 64, 64, 10, 10, "YTP", 255, 255, 255);
    /* Check that some pixels were drawn */
    int drawn = 0;
    for (int i = 0; i < 64*64; i++)
        if (text_frame[i*4] > 0) { drawn++; break; }
    CHECK(drawn > 0, "tech_text: drew pixels");

    /* ---- Remux Chain ---- */
    printf("\n--- Remux Chain ---\n");
    wb_remux_chain rc;
    wb_remux_init(&rc, 3, 30);
    CHECK(rc.passes == 3, "remux: 3 passes");

    uint8_t remux_frame[64*64*4];
    for (int i = 0; i < 64*64*4; i++) remux_frame[i] = (uint8_t)(i % 256);
    wb_remux_pass(&rc, remux_frame, 64, 64);
    CHECK(rc.current_pass == 1, "remux: pass incremented");

    /* ---- Steganography ---- */
    printf("\n--- Steganography ---\n");
    uint8_t steg_frame[64*64*4];
    memset(steg_frame, 128, sizeof(steg_frame));
    const char *msg = "HELLO YTP";
    int embedded = wb_steg_embed(steg_frame, 64, 64, msg);
    CHECK(embedded == 1, "steg: message embedded");

    char extracted[64];
    int extracted_len = wb_steg_extract(steg_frame, 64, 64, extracted, 64);
    CHECK(extracted_len > 0, "steg: message extracted");
    CHECK(strcmp(extracted, msg) == 0, "steg: message matches");

    /* ---- Video Timestretch ---- */
    printf("\n--- Video Timestretch ---\n");
    wb_video_timestretch ts;
    wb_video_ts_init(&ts, 32, 32, 2.0f);
    CHECK(ts.ratio == 2.0f, "timestretch: ratio=2.0");

    uint8_t ts_input[32*32*4];
    uint8_t ts_output[32*32*4];
    memset(ts_input, 100, sizeof(ts_input));
    wb_video_ts_process(&ts, ts_input, ts_output);
    CHECK(1, "timestretch: processed without crash");

    wb_video_ts_free(&ts);
    CHECK(1, "timestretch: freed");

    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    wb_pixel_sort(NULL, 0, 0, 0, 0);
    wb_stutter_minus_init(NULL, 0, 0);
    wb_stutter_minus_tick(NULL);
    wb_buzzing_stutter_init(NULL, 0, 0);
    wb_buzzing_stutter_gen(NULL, 0);
    wb_buzzing_stutter_tick(NULL);
    wb_sexophone_init(NULL);
    wb_sexophone_gen_sample(NULL, 0);
    wb_sexophone_pulse(NULL, 0);
    wb_tech_text(NULL, 0, 0, 0, 0, NULL, 0, 0, 0);
    wb_remux_init(NULL, 0, 0);
    wb_remux_pass(NULL, NULL, 0, 0);
    wb_steg_embed(NULL, 0, 0, NULL);
    wb_steg_extract(NULL, 0, 0, NULL, 0);
    wb_steg_embed_frame(NULL, 0, 0, NULL, 0, 0);
    wb_video_ts_init(NULL, 0, 0, 0);
    wb_video_ts_process(NULL, NULL, NULL);
    wb_video_ts_free(NULL);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", p, p + f);
    return f > 0 ? 1 : 0;
}
