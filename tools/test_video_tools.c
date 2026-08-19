/* test_video_tools.c — headless verification of split-clip + auto-clip-shorts
 * (R015 Tier 3, EDIT-tab ^X). Uses the real test clip /tmp/vidtest/src.mp4. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "wbus/wbus.h"
#include "wbus/wbus_video.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

static const char *SRC = "/tmp/vidtest/src.mp4";
static const char *OUT = "/tmp/vidtest/shorts";

int main(void) {
    printf("=== Video tools test (split + auto-clip) ===\n\n");

    /* ---- split clip ---- */
    printf("-- split clip --\n");
    wb_session *s = wb_session_create();
    int vt = wb_session_add_video_track(s, "V1");
    int clip = wb_session_add_video_clip(s, vt, SRC, 0.0);
    CHECK(clip >= 0, "video clip added");
    double dur = s->tracks[vt].clips[clip].video->duration;
    CHECK(dur > 2.0, "clip has real duration > 2s");
    printf("         source duration = %.2f s\n", dur);

    double split = dur * 0.5;
    int r = wb_session_split_video_clip(s, vt, clip, split);
    CHECK(r > clip, "split returned new right clip index");
    if (r > clip) {
        double llen = s->tracks[vt].clips[clip].length;
        double rlen = s->tracks[vt].clips[r].length;
        CHECK(fabs((llen + rlen) - dur) < 1e-3, "left+right length == original");
        CHECK(fabs(s->tracks[vt].clips[r].start - split) < 1e-6,
              "right clip starts at split point");
        CHECK(fabs(s->tracks[vt].clips[r].video->start_in_source - (split))
              < 1e-6, "right clip source offset shifted by split");
        CHECK(s->tracks[vt].clip_count == 2, "track now has 2 clips");
    }

    wb_session_destroy(s);

    /* ---- auto clip to shorts ---- */
    printf("\n-- auto clip to shorts --\n");
    char mk[] = "/tmp/vidtest/shorts";
    mkdir(mk, 0755);
    int n = wb_video_auto_clip_shorts(SRC, OUT, 0.3, 0.5, 90.0);
    printf("         exported clips = %d\n", n);
    CHECK(n >= 0, "auto-clip returned (>=0)");
    CHECK(n > 0, "auto-clip produced at least one short");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
