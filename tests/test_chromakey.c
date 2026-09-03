/* test_chromakey.c — verify chroma key engine + ffmpeg chroma key wrappers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

/* Generate test video with green background + red circle (YTP-style) */
static int make_greenscreen_video(const char *path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error "
             "-f lavfi -i \"color=c=0x00FF00:s=320x240:r=30:d=2\" "
             "-f lavfi -i \"color=c=0xFF0000:s=80x80:r=30:d=2\" "
             "-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=2\" "
             "-filter_complex \"[1:v]format=yuva420p[circle];"
             "[0:v][circle]overlay=120:80[bg];"
             "[bg]format=yuv420p[outv]\" "
             "-map \"[outv]\" -map 2:a "
             "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -b:a 64k -shortest "
             "\"%s\" 2>&1", path);
    return system(cmd);
}

/* Generate background video */
static int make_background_video(const char *path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -hide_banner -loglevel error "
             "-f lavfi -i \"color=c=0x0000FF:s=320x240:r=30:d=2\" "
             "-f lavfi -i \"sine=frequency=880:sample_rate=44100:duration=2\" "
             "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -b:a 64k -shortest "
             "\"%s\" 2>&1", path);
    return system(cmd);
}

int main(void) {
    int pass = 0, fail = 0;
    printf("=== Chroma Key Engine + FFmpeg Wrappers ===\n");

    /* ---- Engine-level chroma key ---- */
    printf("\n--- Engine-level ---\n");

    void *keyer = wb_chromakey_create(320, 240);
    CHECK(keyer != NULL, "chromakey engine created");

    if (keyer) {
        wb_chromakey_set_key_color(keyer, 0.0f, 1.0f, 0.0f);  /* Green */
        wb_chromakey_set_threshold(keyer, 0.4f);
        wb_chromakey_set_softness(keyer, 0.1f);

        /* Create test frame: green pixel + red pixel */
        uint8_t fg[4 * 4];  /* 2x2 test */
        uint8_t out[4 * 4];

        /* Pixel 0: pure green (should be keyed out) */
        fg[0] = 0; fg[1] = 255; fg[2] = 0; fg[3] = 255;
        /* Pixel 1: pure red (should be kept) */
        fg[4] = 255; fg[5] = 0; fg[6] = 0; fg[7] = 255;
        /* Pixel 2: dark green (should be keyed) */
        fg[8] = 0; fg[9] = 180; fg[10] = 0; fg[11] = 255;
        /* Pixel 3: yellow (should be kept) */
        fg[12] = 255; fg[13] = 255; fg[14] = 0; fg[15] = 255;

        wb_chromakey_process(keyer, fg, out, 2, 2);

        CHECK(out[3] == 0, "pure green keyed out (alpha=0)");
        CHECK(out[7] == 255, "pure red kept (alpha=255)");
        CHECK(out[11] == 0, "dark green keyed out (alpha=0)");
        CHECK(out[15] == 255, "yellow kept (alpha=255)");

        wb_chromakey_destroy(keyer);
    }

    /* ---- FFmpeg chroma key wrappers ---- */
    printf("\n--- FFmpeg wrappers ---\n");

    int rc = make_greenscreen_video("/tmp/wb_greenscreen.mp4");
    CHECK(rc == 0, "greenscreen test video created");

    rc = make_background_video("/tmp/wb_background.mp4");
    CHECK(rc == 0, "background test video created");

    /* Basic chromakey */
    rc = wb_chromakey("/tmp/wb_greenscreen.mp4", "/tmp/wb_keyed.webm",
                       0x00FF00, 0.3, 0.1, NULL);
    CHECK(rc == 0, "basic chromakey");

    FILE *f = fopen("/tmp/wb_keyed.webm", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        CHECK(sz > 1000, "keyed output has content");
        printf("  Keyed output: %ld bytes\n", sz);
    } else {
        printf("  FAIL: keyed output not found\n"); fail++;
    }

    /* Chromakey composite */
    rc = wb_chromakey_composite("/tmp/wb_greenscreen.mp4", "/tmp/wb_background.mp4",
                                 "/tmp/wb_composite.mp4",
                                 0x00FF00, 0.3, 0.1, 0, 0, 1.0);
    CHECK(rc == 0, "chromakey composite");

    /* Pro chromakey with feather + erode */
    rc = wb_chromakey_pro("/tmp/wb_greenscreen.mp4", "/tmp/wb_keyed_pro.webm",
                           0x00FF00, 0.3, 0.05, 2.0, 1.0, 0);
    CHECK(rc == 0, "pro chromakey with feather+erode");

    /* Pro composite */
    rc = wb_chromakey_pro_composite("/tmp/wb_greenscreen.mp4", "/tmp/wb_background.mp4",
                                     "/tmp/wb_pro_composite.mp4",
                                     0x00FF00, 0.3, 0.05, 2.0, 1.0, 0, 0, 1.0);
    CHECK(rc == 0, "pro chromakey composite");

    /* Keylight pro */
    rc = wb_keylight_pro("/tmp/wb_greenscreen.mp4", "/tmp/wb_keylight.webm",
                          0x00FF00, 1.0, 0.5, 1.0, 0.5, 1.0, 2.0);
    CHECK(rc == 0, "keylight pro");

    /* ---- Scene detection ---- */
    printf("\n--- Scene detection ---\n");

    /* Create a video with scene cuts */
    {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -hide_banner -loglevel error "
                 "-f lavfi -i \"color=c=red:s=320x240:r=30:d=1\" "
                 "-f lavfi -i \"color=c=blue:s=320x240:r=30:d=1\" "
                 "-f lavfi -i \"color=c=green:s=320x240:r=30:d=1\" "
                 "-f lavfi -i \"color=c=white:s=320x240:r=30:d=1\" "
                 "-filter_complex \"[0:v][1:v][2:v][3:v]concat=n=4:v=1:a=0[outv]\" "
                 "-map \"[outv]\" -c:v libx264 -preset ultrafast "
                 "/tmp/wb_scenes.mp4 2>&1");
        rc = system(cmd);
        CHECK(rc == 0, "scene test video created (4 cuts)");
    }

    wb_scene_list *scenes = wb_scene_detect_ffmpeg("/tmp/wb_scenes.mp4", 0.3);
    CHECK(scenes != NULL, "scene detection returned list");
    if (scenes) {
        printf("  Detected %d scene cuts\n", scenes->count);
        CHECK(scenes->count >= 3, "detected at least 3 of 4 cuts");
        for (int i = 0; i < scenes->count && i < 10; i++) {
            printf("    Cut %d: t=%.2fs score=%.3f\n", i,
                   scenes->cuts[i].timestamp, scenes->cuts[i].scene_score);
        }
        wb_scene_list_ffmpeg_free(scenes);
    }

    /* ---- Edge detection ---- */
    printf("\n--- Edge detection ---\n");

    rc = wb_edge_detect("/tmp/wb_greenscreen.mp4", "/tmp/wb_edges.mp4", 1, 0.1);
    CHECK(rc == 0, "sobel edge detection");

    /* ---- Content-aware fill ---- */
    printf("\n--- Content-aware fill ---\n");

    rc = wb_content_aware_fill("/tmp/wb_greenscreen.mp4", "/tmp/wb_filled.mp4",
                                120, 80, 80, 80, 0);
    CHECK(rc == 0, "content-aware fill (delogo)");

    /* ---- Depth pseudo ---- */
    printf("\n--- Pseudo depth ---\n");

    rc = wb_depth_pseudo("/tmp/wb_greenscreen.mp4", "/tmp/wb_depth.mp4", 0.5, 5.0);
    CHECK(rc == 0, "pseudo depth map");

    /* ---- Speed / Reverse (from R092) ---- */
    printf("\n--- Speed+Reverse ---\n");

    extern int wb_ffmpeg_speed(const char *, const char *, double);
    extern int wb_ffmpeg_reverse(const char *, const char *, int);

    rc = wb_ffmpeg_speed("/tmp/wb_greenscreen.mp4", "/tmp/wb_speed.mp4", 2.0);
    CHECK(rc == 0, "speed 2x");

    rc = wb_ffmpeg_reverse("/tmp/wb_greenscreen.mp4", "/tmp/wb_rev.mp4", 0);
    CHECK(rc == 0, "reverse video");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
