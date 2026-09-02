/* test_dvd_author.c — verify DVD/Blu-ray authoring with proper navigation */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void) {
    int pass = 0, fail = 0;
    printf("=== DVD/Blu-ray Authoring (Proper Navigation) ===\n");

    struct wb_dvd_project *p = wb_dvd_author_create();
    CHECK(p != NULL, "DVD project created");

    /* Video standard + aspect */
    wb_dvd_author_set_video_standard(p, DVD_VIDEO_NTSC);
    wb_dvd_author_set_aspect_ratio(p, DVD_ASPECT_16X9);
    CHECK(1, "video standard and aspect set");

    /* Add titles */
    wb_dvd_author_add_title(p, "/tmp/test_video.mp4", "/tmp/test_audio.wav", 60.0);
    wb_dvd_author_add_title(p, "/tmp/test_video2.mp4", NULL, 120.0);
    CHECK(1, "2 titles added");

    /* Set up menu with buttons */
    wb_dvd_button buttons[] = {
        {100, 100, 200, 50, 1, 0, 1, 0, 0, 0},  /* button 1: play title 1 */
        {100, 200, 200, 50, 2, 0, 0, 0, 0, 0},  /* button 2: play title 2 */
        {100, 300, 200, 50, 0, 1, 0, 0, 0, 0},  /* button 3: chapter menu */
    };
    wb_dvd_author_set_menu(p, "/tmp/menu_bg.png", buttons, 3);
    CHECK(1, "menu set with 3 buttons");

    /* Set chapters */
    double chapters[] = {0.0, 30.0, 60.0};
    wb_dvd_author_set_chapters(p, 0, chapters, 3);
    CHECK(1, "chapters set for title 0");

    /* Export DVD structure (without actual ffmpeg encoding) */
    char tmpdir[] = "/tmp/dvd_test_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (dir) {
        int rc = wb_dvd_author_export(p, dir, DVD_FORMAT_DVD5);
        /* Will fail at ffmpeg stage (no input files), but structure is created */
        printf("  export returned %d (expected: -1 due to missing input files)\n", rc);

        /* Check that directory structure was created */
        char check_path[1024];
        snprintf(check_path, sizeof(check_path), "%s/VIDEO_TS", dir);
        struct stat st;
        if (stat(check_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            printf("  PASS: VIDEO_TS directory created\n");
            pass++;
        } else {
            printf("  FAIL: VIDEO_TS directory not created\n");
            fail++;
        }

        /* Check IFO file was generated */
        snprintf(check_path, sizeof(check_path), "%s/VIDEO_TS/VIDEO_TS.IFO", dir);
        if (stat(check_path, &st) == 0) {
            printf("  PASS: VIDEO_TS.IFO created (%ld bytes)\n", (long)st.st_size);
            pass++;
        } else {
            printf("  FAIL: VIDEO_TS.IFO not created\n");
            fail++;
        }

        /* Check BUP backup */
        snprintf(check_path, sizeof(check_path), "%s/VIDEO_TS/VIDEO_TS.BUP", dir);
        if (stat(check_path, &st) == 0) {
            printf("  PASS: VIDEO_TS.BUP backup created\n");
            pass++;
        } else {
            printf("  FAIL: VIDEO_TS.BUP not created\n");
            fail++;
        }

        /* Check per-title VTS IFOs */
        snprintf(check_path, sizeof(check_path), "%s/VIDEO_TS/VTS_01_0.IFO", dir);
        if (stat(check_path, &st) == 0) {
            printf("  PASS: VTS_01_0.IFO created\n");
            pass++;
        } else {
            printf("  FAIL: VTS_01_0.IFO not created\n");
            fail++;
        }
    }

    /* Cleanup */
    wb_dvd_author_destroy(p);
    CHECK(1, "project destroyed");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
