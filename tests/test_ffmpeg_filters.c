/* test_ffmpeg_filters.c — verify advanced ffmpeg filter_complex wrappers */
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

/* Generate a small test video WITH audio */
static int make_test_video(const char *path, int w, int h, int fps, double dur, const char *color) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error "
             "-f lavfi -i \"color=c=%s:s=%dx%d:r=%d:d=%.1f\" "
             "-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=%.1f\" "
             "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -b:a 64k -shortest "
             "\"%s\" 2>&1",
             color, w, h, fps, dur, dur, path);
    return system(cmd);
}

int main(void) {
    int pass = 0, fail = 0;
    printf("=== Advanced FFmpeg Filter Complex ===\n");

    /* Create test clips with audio */
    int rc = make_test_video("/tmp/wb_test_a.mp4", 320, 240, 30, 2, "red");
    CHECK(rc == 0, "test clip A created (red, with audio)");
    rc = make_test_video("/tmp/wb_test_b.mp4", 320, 240, 30, 2, "blue");
    CHECK(rc == 0, "test clip B created (blue, with audio)");

    /* Test transition */
    rc = wb_ffmpeg_transition("/tmp/wb_test_a.mp4", "/tmp/wb_test_b.mp4",
                               "/tmp/wb_test_xfade.mp4", WB_XFADE_WIPELEFT, 0.5, 1.0);
    CHECK(rc == 0, "xfade wipeleft transition");

    /* Verify output */
    FILE *f = fopen("/tmp/wb_test_xfade.mp4", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        CHECK(sz > 1000, "transition output has content");
        printf("  Transition output: %ld bytes\n", sz);
    } else {
        printf("  FAIL: transition output not found\n"); fail++;
    }

    /* Test probe */
    int pw = 0, ph = 0;
    double pdur = 0, pfps = 0;
    rc = wb_ffmpeg_probe("/tmp/wb_test_a.mp4", &pw, &ph, &pdur, &pfps);
    CHECK(rc == 0, "probe succeeded");
    printf("  Probed: %dx%d %.1fs @ %.0ffps\n", pw, ph, pdur, pfps);
    CHECK(pw == 320 && ph == 240, "probe dimensions correct");

    /* Test speed */
    rc = wb_ffmpeg_speed("/tmp/wb_test_a.mp4", "/tmp/wb_test_speed.mp4", 2.0);
    CHECK(rc == 0, "speed 2x");

    /* Test reverse */
    rc = wb_ffmpeg_reverse("/tmp/wb_test_a.mp4", "/tmp/wb_test_rev.mp4", 1);
    CHECK(rc == 0, "reverse video+audio");

    /* Test color grade */
    rc = wb_ffmpeg_color_grade("/tmp/wb_test_a.mp4", "/tmp/wb_test_grade.mp4", 0.1, 1.2, 1.5, 0.0);
    CHECK(rc == 0, "color grade");

    /* Test fade */
    rc = wb_ffmpeg_fade("/tmp/wb_test_a.mp4", "/tmp/wb_test_fade.mp4", 0.3, 1.5, 0.3);
    CHECK(rc == 0, "fade in/out");

    /* Test concat */
    const char *clips[] = {"/tmp/wb_test_a.mp4", "/tmp/wb_test_b.mp4"};
    rc = wb_ffmpeg_concat(clips, 2, "/tmp/wb_test_concat.mp4");
    CHECK(rc == 0, "concat 2 clips");

    /* Test blend */
    rc = wb_ffmpeg_blend("/tmp/wb_test_a.mp4", "/tmp/wb_test_b.mp4",
                          "/tmp/wb_test_blend.mp4", 1 /* multiply */, 0.5, 0);
    CHECK(rc == 0, "blend multiply");

    /* Test xfade name lookup */
    CHECK(strcmp(wb_xfade_name(WB_XFADE_WIPELEFT), "wipeleft") == 0, "xfade name lookup");
    CHECK(strcmp(wb_xfade_name(WB_XFADE_DISSOLVE), "dissolve") == 0, "xfade dissolve name");
    CHECK(strcmp(wb_xfade_name(999), "fade") == 0, "xfade out-of-range fallback");

    /* Test blend name lookup */
    CHECK(strcmp(wb_blend_name(1), "multiply") == 0, "blend name lookup");
    CHECK(strcmp(wb_blend_name(999), "normal") == 0, "blend out-of-range fallback");

    /* Test transition chain (3 clips) */
    make_test_video("/tmp/wb_test_c.mp4", 320, 240, 30, 2, "green");
    const char *chain_clips[] = {"/tmp/wb_test_a.mp4", "/tmp/wb_test_b.mp4", "/tmp/wb_test_c.mp4"};
    rc = wb_ffmpeg_transition_chain(chain_clips, 3, "/tmp/wb_test_chain.mp4", WB_XFADE_DISSOLVE, 0.5);
    CHECK(rc == 0, "3-clip transition chain");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
