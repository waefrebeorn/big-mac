/* test_fcpxml.c — R018-C FCPXML intent enrichment verification.
 *
 * Builds a session with a color-corrected video clip + an audio track,
 * exports FCPXML, and asserts the interchange now carries color-correction
 * intent (<adjust-color>), audio roles (audioRole=), and volume
 * (<adjust-volume>). This is the "transfer intent, not just data" gap. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus.h"

static int failures = 0, checks = 0;
#define CK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== R018-C FCPXML intent enrichment ===\n\n");

    wb_session *s = wb_session_create();
    /* video track + one color-corrected clip */
    int vt = wb_session_add_video_track(s, "V1");
    int vc = wb_session_add_video_clip(s, vt, "/tmp/fcpx_src.mp4", 0.0);
    CK(vc >= 0, "video clip added");
    if (vc >= 0) {
        /* reach the clip and set color intent (exposure +1 stop, sat 1.2) */
        wb_track *tr = &s->tracks[vt];
        wb_clip *cl = &tr->clips[vc];
        wb_clip_set_color(cl, 1.0f, 1.2f);
    }

    /* audio track named 'dialogue' + one clip */
    wb_track *at = wb_session_add_track(s, "dialogue", WB_TRACK_KIND_AUDIO);
    /* a 1s silence buffer (44100 frames, mono) */
    float *buf = (float*)calloc(44100, sizeof(float));
    int ac = wb_session_add_audio_clip(at, 0.0, 44100.0, buf, 44100, 1);
    free(buf);
    CK(ac >= 0, "audio clip added");
    at->volume = 0.5f;   /* -6 dB intent */

    const char *xml = "/tmp/test_fcpxml.fcpxml";
    remove(xml);
    CK(wb_session_export_fcpxml(s, xml) == 0, "FCPXML export succeeded");

    FILE *f = fopen(xml, "r");
    CK(f != NULL, "FCPXML file exists");
    int has_adjust_color = 0, has_role = 0, has_vol = 0, has_spine = 0;
    char line[1024];
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "<spine>")) has_spine++;
            if (strstr(line, "adjust-color")) has_adjust_color++;
            if (strstr(line, "audioRole=")) has_role++;
            if (strstr(line, "adjust-volume")) has_vol++;
        }
        fclose(f);
    }
    CK(has_spine == 1, "FCPXML has a <spine>");
    CK(has_adjust_color >= 1, "FCPXML carries <adjust-color> color intent");
    CK(has_role >= 1, "FCPXML carries audioRole (dialogue) intent");
    CK(has_vol >= 1, "FCPXML carries <adjust-volume> gain intent");

    /* inspect the actual adjust-color values */
    f = fopen(xml, "r");
    if (f) {
        int found_ex = 0, found_sat = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "ex=\"+1.000\"") || strstr(line, "ex=\"1.000\"")) found_ex++;
            if (strstr(line, "sat=\"1.200\"")) found_sat++;
        }
        fclose(f);
        CK(found_ex >= 1, "adjust-color exposure == +1.0 stop");
        CK(found_sat >= 1, "adjust-color saturation == 1.2");
    } else {
        CK(0, "adjust-color values inspected");
    }

    wb_session_destroy(s);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
