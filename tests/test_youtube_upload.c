/* test_youtube_upload.c — verify YouTube upload API */
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
    printf("=== YouTube Upload ===\n");

    /* Set credentials */
    int rc = wb_youtube_set_credentials("test_client_id", "test_secret", "test_refresh");
    CHECK(rc == 0, "credentials set");

    /* Upload without credentials should fail */
    rc = wb_youtube_upload(NULL, "title", "desc", "tags", 0);
    CHECK(rc == -1, "NULL video path returns -1");

    /* Upload with nonexistent file */
    rc = wb_youtube_upload("/tmp/nonexistent.mp4", "title", "desc", "tags", 0);
    CHECK(rc == -3, "nonexistent file returns -3");

    /* Create temp file for upload test */
    FILE *tf = fopen("/tmp/test_upload.mp4", "w");
    if (tf) { fclose(tf); }
    /* Upload with valid credentials (stub returns 0) */
    rc = wb_youtube_upload("/tmp/test_upload.mp4", "Test Video", "Description", "tag1,tag2", 0);
    CHECK(rc >= 0, "upload returns success (stub)");
    remove("/tmp/test_upload.mp4");

    /* Status */
    double progress = 0;
    char status[256];
    rc = wb_youtube_get_upload_status(&progress, status, sizeof(status));
    CHECK(rc == 0, "status query succeeds");

    /* Cancel */
    rc = wb_youtube_cancel_upload();
    CHECK(rc == 0, "cancel succeeds");

    /* Thumbnail */
    rc = wb_youtube_set_thumbnail("/tmp/test.mp4", "/tmp/thumb.png");
    CHECK(rc == 0, "thumbnail set (stub)");

    /* NULL safety */
    wb_youtube_set_credentials(NULL, NULL, NULL);
    wb_youtube_get_upload_status(NULL, NULL, 0);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
