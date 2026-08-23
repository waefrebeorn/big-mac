/* test_perf_freeze.c — R068 end-to-end: recorded perf → timeline clip →
 * agent `perf-freeze` command produces a type=3 clip in the session. */

#include <stdio.h>
#include <stdlib.h>
#include "wbus/wbus.h"
#include "wbus/wbus_perf.h"
#include "wbus/wbus_perfclip.h"
#include "wbus/wbus_agent.h"
#include "wbus/wbus_shadowbin.h"

static int failures=0, checks=0;
#define CHECK(c,m) do{checks++; if(c) printf("  [PASS] %s\n",m); else {printf("  [FAIL] %s\n",m); failures++;}}while(0)

int main(void){
    printf("=== Perf → timeline clip (R068) test ===\n\n");
    wb_session *s = wb_session_create(); s->length = 44100.0*4;
    wb_perf *perf = wb_perf_create(64, 64);
    wb_mesh *m = wb_mesh_box(1,1,0.2f,255,0,0);
    CHECK(wb_perf_add_deck(perf, m, 255,0,0) == 0, "deck 0 added");

    /* record a 1s phrase: fire at t0, fade at t=0.5 */
    wb_perf_set_clock(perf, 0.0);
    wb_perf_record_arm(perf);
    wb_perf_fire(perf, 0);
    wb_perf_set_clock(perf, 0.5);
    wb_perf_fade(perf, 0.3f);
    wb_perf_record_stop(perf);
    CHECK(wb_perf_event_count(perf) == 2, "2 events recorded");

    wb_agent_set_perf(perf);
    int vt = wb_session_add_video_track(s, "VJ");
    CHECK(vt >= 0, "video track added");

    /* agent freezes the perf onto track vt @ 1.0s for 2s */
    int rc = wb_agent_command(s, NULL, "perf-freeze 0 1.0 2.0");
    CHECK(rc == 0, "perf-freeze command succeeds");

    wb_track *tr = &s->tracks[vt];
    CHECK(tr->clip_count == 1, "one clip now on video track");
    wb_clip *cl = &tr->clips[0];
    CHECK(cl->type == 3, "clip type == 3 (performance)");
    CHECK(cl->perfclip != NULL, "clip owns a perfclip snapshot");
    CHECK(fabs(cl->start - 1.0*WB_SAMPLE_RATE) < 1.0, "clip start at 1.0s");
    CHECK(fabs(cl->length - 2.0) < 0.01, "clip duration 2.0s");

    /* the snapshot is independent: free the live perf, clip still stands */
    wb_perf_free(perf);
    CHECK(wb_perfclip_event_count((wb_perfclip*)cl->perfclip) == 2,
          "clip retains 2 events after live perf freed (self-contained)");

    /* JSON sidecar round-trips the clip envelope (not the full event log,
     * but enough for interchange: type + timing). */
    wb_shadowbin_write(s, "/tmp/perf_freeze_sidecar.json");
    FILE *f = fopen("/tmp/perf_freeze_sidecar.json","r");
    CHECK(f != NULL, "shadow bin saved with perf clip");
    if (f) {
        char buf[2048]; size_t n=fread(buf,1,2047,f); buf[n]=0;
        CHECK(strstr(buf,"perfclip")!=NULL, "sidecar tags clip as perfclip");
        fclose(f);
    }

    wb_session_destroy(s);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures==0?0:1;
}
