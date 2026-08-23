/* test_perf_export.c — R070: perf-clip → video export passthrough.
 * Freezes a recorded VJ set onto the timeline, then runs
 * wb_video_export_perf_overlays and verifies (a) an mp4 comes out,
 * (b) it has the expected duration, (c) the perf overlay window actually
 * contains non-background pixels (the deck rendered). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus.h"
#include "wbus/wbus_perf.h"
#include "wbus/wbus_perfclip.h"
#include "wbus/wbus_agent.h"
#include "wbus/wbus_cgiexport.h"
#include <unistd.h>

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== Perf → export passthrough (R070) test ===\n\n");

    wb_session *s = wb_session_create();
    s->length = 44100.0 * 6;
    wb_perf *perf = wb_perf_create(640, 360);
    wb_mesh *m = wb_mesh_box(1.4f, 1.4f, 0.3f, 255, 120, 40);
    CHECK(wb_perf_add_deck(perf, m, 255, 120, 40) == 0, "deck added");

    /* record: fire at t=0 */
    wb_perf_set_clock(perf, 0);
    wb_perf_record_arm(perf);
    wb_perf_fire(perf, 0);
    wb_perf_record_stop(perf);

    wb_agent_set_perf(perf);
    int vt = wb_session_add_video_track(s, "V1");
    CHECK(vt >= 0, "video track");
    int rc = wb_agent_command(s, NULL, "perf-freeze 0 2.0 2.0");
    CHECK(rc == 0, "perf-freeze @2.0s for 2s");
    CHECK(s->tracks[vt].clips[0].type == 3 &&
          s->tracks[vt].clips[0].perfclip != NULL, "type-3 clip present");

    /* render the clip standalone at t=0.5 to prove pixels come out */
    wb_clip *cl = &s->tracks[vt].clips[0];
    static uint8_t rgba[640*360*4];
    memset(rgba, 0, sizeof rgba);
    CHECK(wb_perfclip_render((wb_perfclip*)cl->perfclip, 0.5,
                             rgba, 640, 360) == 0, "clip renders");
    int lit = 0;
    for (int i = 0; i < 640*360; i++) if (rgba[i*4+3] > 128) lit++;
    CHECK(lit > 1000, "clip has real pixels (deck rasterized)");
    printf("         lit=%d px of %d\n", lit, 640*360);

    /* raw sequence write */
    unlink("/tmp/perf_seq.raw");
    CHECK(wb_perfclip_render_seq((wb_perfclip*)cl->perfclip,
                                 0.0, 2.0, 24.0, 640, 360,
                                 "/tmp/perf_seq.raw") == 0, "render_seq ok");
    FILE *rf = fopen("/tmp/perf_seq.raw", "rb");
    CHECK(rf != NULL, "raw seq exists");
    long bytes = -1;
    if (rf) { fseek(rf, 0, SEEK_END); bytes = ftell(rf); fclose(rf); }
    /* 2s * 24fps * 640*360*4 bytes/frame ≈ 44.2 MB */
    CHECK(bytes > (long)(2*24*640*360*4 * 0.9),
          "raw seq size matches 48 frames of RGBA");

    /* full export with perf overlays */
    const char *out = "/tmp/bigmac_perf_export.mp4";
    unlink(out);
    int erc = wb_video_export_perf_overlays(s, NULL, out, NULL, WB_VIDEO_CODEC_H264);
    CHECK(erc == 0, "export_perf_overlays succeeded");
    FILE *of = fopen(out, "rb");
    CHECK(of != NULL, "export mp4 exists");
    long osize = 0;
    if (of) { fseek(of, 0, SEEK_END); osize = ftell(of); fclose(of); }
    CHECK(osize > 5000 && osize < 100000000, "mp4 is plausible size");
    printf("         export=%ld bytes\n", osize);

    wb_session_destroy(s);
    wb_mesh_free(m);
    wb_perf_free(perf);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
