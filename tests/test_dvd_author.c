/* test_dvd_author.c — verify DVD/Blu-ray authoring */
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
    printf("=== DVD/Blu-ray Authoring ===\n");

    struct wb_dvd_project *p = wb_dvd_author_create();
    CHECK(p != NULL, "DVD project created");

    /* Add titles */
    wb_dvd_author_add_title(p, "/tmp/test_video.mp4", "/tmp/test_audio.wav", 60.0);
    CHECK(1, "title 1 added");

    wb_dvd_author_add_title(p, "/tmp/test_video2.mp4", NULL, 120.0);
    CHECK(1, "title 2 added (no audio)");

    /* Set menu */
    wb_dvd_button buttons[] = {
        {100, 100, 200, 50, 0},
        {100, 200, 200, 50, 1},
    };
    wb_dvd_author_set_menu(p, "/tmp/menu_bg.png", buttons, 2);
    CHECK(1, "menu set with 2 buttons");

    /* Set chapters */
    double chapters[] = {0.0, 30.0, 60.0};
    wb_dvd_author_set_chapters(p, 0, chapters, 3);
    CHECK(1, "chapters set for title 0");

    /* Export DVD */
    char tmpdir[] = "/tmp/dvd_test_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (dir) {
        int rc = wb_dvd_author_export(p, dir, WB_DVD_FORMAT_DVD5);
        CHECK(rc == 0, "DVD export succeeded");
    }

    /* Cleanup */
    wb_dvd_author_destroy(p);
    CHECK(1, "project destroyed");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
