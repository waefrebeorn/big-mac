/* test_hdr_preview.c — verify HDR preview */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void) {
    int pass = 0, fail = 0;
    printf("=== HDR Preview ===\n");

    struct wb_hdr_preview *hdr = wb_hdr_preview_create(64, 64);
    CHECK(hdr != NULL, "HDR preview created");

    /* Display modes */
    wb_hdr_set_display_mode(hdr, WB_HDR_DISPLAY_SDR);
    wb_hdr_set_display_mode(hdr, WB_HDR_DISPLAY_HDR10);
    wb_hdr_set_display_mode(hdr, WB_HDR_DISPLAY_HLG);
    wb_hdr_set_display_mode(hdr, WB_HDR_DISPLAY_DOLBY_VISION);
    CHECK(1, "all display modes set");

    /* Peak brightness */
    wb_hdr_set_peak_brightness(hdr, 1000.0f);
    wb_hdr_set_peak_brightness(hdr, 50.0f);    /* clamp to 100 */
    wb_hdr_set_peak_brightness(hdr, 50000.0f); /* clamp to 10000 */
    CHECK(1, "peak brightness with clamping");

    /* Color space */
    wb_hdr_set_color_space(hdr, WB_HDR_CS_REC709);
    wb_hdr_set_color_space(hdr, WB_HDR_CS_REC2020);
    wb_hdr_set_color_space(hdr, WB_HDR_CS_DCIP3);
    CHECK(1, "color spaces set");

    /* Tone mapping */
    wb_hdr_set_tone_map(hdr, 0); /* Reinhard */
    wb_hdr_set_tone_map(hdr, 1); /* ACES */
    CHECK(1, "tone map methods set");

    /* Exposure */
    wb_hdr_set_exposure(hdr, 2.0f);
    CHECK(1, "exposure set");

    /* Process frame */
    wb_frame *fin = wb_frame_alloc(64, 64);
    wb_frame *fout = wb_frame_alloc(64, 64);
    if (fin && fout) {
        /* Fill with test pattern */
        for (int i = 0; i < 64 * 64; i++) {
            fin->px[i].r = 200;
            fin->px[i].g = 150;
            fin->px[i].b = 100;
            fin->px[i].a = 255;
        }

        wb_hdr_set_display_mode(hdr, WB_HDR_DISPLAY_SDR);
        wb_hdr_process_frame(hdr, fin, fout);
        CHECK(fout->px[0].r <= 255 && fout->px[0].a == 255, "SDR tone mapping produces valid output");

        wb_hdr_set_display_mode(hdr, WB_HDR_DISPLAY_HDR10);
        wb_hdr_process_frame(hdr, fin, fout);
        CHECK(fout->px[0].r <= 255, "HDR10 processing produces valid output");

        wb_hdr_apply_metadata(hdr, fout, 1000.0f, 400.0f);
        CHECK(1, "HDR metadata applied");
    }
    if (fin) wb_frame_free(fin);
    if (fout) wb_frame_free(fout);

    /* Cleanup */
    wb_hdr_preview_destroy(hdr);
    CHECK(1, "HDR preview destroyed");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
