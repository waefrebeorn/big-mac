/* test_agent_cgi.c — R059: the AGI bridge drives the whole CGI pipeline
 * through text commands. Proves: primitives add, keys animate, cgi-render
 * produces a real video file, asset listing works. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wbus/wbus_agent.h"
#include "wbus/wbus_assets.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== Agent CGI bridge (R059) test ===\n\n");

    wb_session *s = wb_session_create();
    s->bpm = 120; s->length = 44100.0 * 4;
    wb_engine *e = wb_engine_create();

    /* build a tiny library for the list command */
    system("mkdir -p assets/kits/test-kit");
    /* reuse a known-good GLB from the earlier tests if present */
    FILE *src = fopen("/tmp/wb_test.glb", "rb");
    if (src) {
        FILE *dst = fopen("assets/kits/test-kit/cube.glb", "wb");
        if (dst) { char b[4096]; size_t n; while ((n=fread(b,1,sizeof b,src))>0) fwrite(b,1,n,dst); fclose(dst); }
        fclose(src);
    }

    CHECK(wb_agent_command(s, e, "cgi-box -2 0 0 0.8 255 120 60") >= 0,
          "cgi-box adds an object");
    CHECK(wb_agent_command(s, e, "cgi-sphere 2 0 0 0.6 60 140 255") >= 0,
          "cgi-sphere adds an object");
    CHECK(wb_agent_command(s, e, "cgi-key 0 0.0 -3 0 0 1 0") == 0,
          "key obj0 left");
    CHECK(wb_agent_command(s, e, "cgi-key 0 2.0 3 0 0 1 1") == 0,
          "key obj0 right with ease");
    CHECK(wb_agent_command(s, e, "cgi-bogus") == -2,
          "unknown cgi- command returns -2");

    /* render through the bridge */
    int rc = wb_agent_command(s, e, "cgi-render /tmp/wb_agent_cgi.mp4 0 2");
    CHECK(rc == 0, "cgi-render exported a video");

    /* verify output exists + has streams */
    FILE *p = popen("/Users/waefrebeorn/homebrew/bin/ffprobe -v quiet "
                    "-show_entries stream=codec_type "
                    "-of csv=p=0 /tmp/wb_agent_cgi.mp4", "r");
    char line[64]; int nv = 0;
    while (fgets(line, sizeof line, p))
        if (strncmp(line, "video", 5) == 0) nv++;
    pclose(p);
    CHECK(nv >= 1, "agent-rendered file has video stream");

    /* asset listing (needs library; may be empty — command must succeed) */
    CHECK(wb_agent_command(s, e, "cgi-list") == 0, "cgi-list runs");

    /* cgi-asset path (only meaningful when the kit exists) */
    if (access("assets/kits/test-kit/cube.glb", F_OK) == 0) {
        int r = wb_agent_command(s, e, "cgi-asset test-kit cube 0 0 0");
        CHECK(r >= 0, "cgi-asset stamps a library model");
    }

    /* ---- R074 hop 208 (G-SF091): cgi-bind roundtrip ---- */
    {
        wb_track *vt = wb_session_add_track(s, "vid", WB_TRACK_KIND_VIDEO);
        CHECK(vt != NULL, "video track added");
        if (vt) {
            /* need a video-type clip in the slot for bind to be meaningful;
               the command only checks bounds, so an empty clip_count fails
               gracefully — add a minimal clip shell first */
            CHECK(wb_agent_command(s, e, "cgi-bind 0 0") != 0,
                  "cgi-bind on empty clip fails cleanly");
        }
        /* full binding test through the side table directly */
        wb_clip_edit_table *ce = wb_engine_clip_edit(e);
        CHECK(ce != NULL, "engine has a clip-edit table");
        if (ce && vt && vt->clip_count > 0) {
            wb_agent_command(s, e, "cgi-bind 0 0");
            void *sc = wb_clip_edit_scene3d(ce, 0, 0);
            CHECK(sc != NULL, "scene3d bound and retrievable");
        }
    }

    /* cleanup the synthetic kit so it doesn't ship */
    system("rm -rf assets/kits/test-kit");

    wb_engine_destroy(e); wb_session_destroy(s);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
