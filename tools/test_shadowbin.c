/* test_shadowbin.c — R061: shadow bin round-trip.
 * Build a session with video + audio + markers, write sidecar, MUTATE the
 * session (simulating edits on either machine), read the sidecar back,
 * and verify every value restored. Also: atomicity (no .tmp litter),
 * bad-JSON rejection, default path helper. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_shadowbin.h"
#include "wbus/wbus_mesh.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Shadow bin (R061) test ===\n\n");
    const char *path = "/tmp/wb_shadowbin_test.json";

    /* ---- build a rich session ---- */
    wb_session *s = wb_session_create();
    s->bpm = 140.0;
    s->length = 44100.0 * 8;
    if (s->name) snprintf(s->name, 64, "RoundTrip");

    int vt = wb_session_add_video_track(s, "V1");
    CHECK(vt >= 0, "video track created");
    if (vt < 0) { printf("%d checks, %d failures\n", checks, failures); return 1; }
    int ci = wb_session_add_video_clip(s, vt, "/tmp/fake_src_a.mp4", 0.0);
    wb_track *vtr = &s->tracks[vt];
    vtr->clips[0].length = 3.5;

    wb_track *atr_early = wb_session_add_track(s, "Voice", 1);
    uint32_t nf = 44100 * 2;
    wb_sample *buf = calloc(nf, sizeof(wb_sample));
    for (uint32_t i = 0; i < nf; i++)
        buf[i] = (wb_sample)(0.3 * sin(2*M_PI*220.0*i/44100.0));
    wb_track *atr = atr_early;
    wb_session_add_audio_clip(atr, 44100.0, (double)nf, buf, nf, 1);
    atr->clips[0].clip_gain = 0.75f;    /* start already 1s via the call */

    wb_session_add_marker(s, 44100.0 * 2, "Chapter One", 1);
    wb_session_add_marker(s, 44100.0 * 5, "Chapter Two", 0);

    /* ---- WRITE ---- */
    int wr = wb_shadowbin_write(s, path);
    CHECK(wr == 0, "write succeeded");

    char defpath[512];
    wb_shadowbin_path_for("/x/proj.wbus", defpath, sizeof defpath);
    CHECK(strcmp(defpath, "/x/proj.shadowbin.json") == 0,
          "default path helper");

    /* no tmp litter after atomic rename */
    char tmpp[600];
    snprintf(tmpp, sizeof tmpp, "%s.tmp", path);
    FILE *tf = fopen(tmpp, "r");
    CHECK(tf == NULL, "no .tmp left behind (atomic rename)");
    if (tf) fclose(tf);

    /* file parses as JSON-ish (starts with { ends with }) */
    {
        FILE *pf = fopen(path, "r");
        CHECK(pf != NULL, "sidecar exists");
        if (pf) {
            char first = (char)fgetc(pf);
            fseek(pf, -1, SEEK_END);
            char last = (char)fgetc(pf);
            fclose(pf);
            CHECK(first == '{' && last == '}', "JSON braces intact");
        }
    }

    /* ---- MUTATE (simulate edits elsewhere) ---- */
    vtr->clips[0].start = 9.9;
    vtr->clips[0].length = 0.1;
    atr->clips[0].start = 88200.0;
    atr->clips[0].clip_gain = 2.0f;
    s->marker_count = 0;

    /* ---- READ BACK ---- */
    int n = wb_shadowbin_read(s, path);
    CHECK(n >= 2, "read restored at least the two clips");
    printf("         restored=%d clips\n", n);

    CHECK(fabs(vtr->clips[0].length - 3.5) < 0.001,
          "video clip length restored");
    CHECK(fabs(vtr->clips[0].start) < 0.001,
          "video clip start restored");
    CHECK(vtr->clips[0].video &&
          fabs(vtr->clips[0].video->start_in_source) < 0.001,
          "video in-point restored");
    CHECK(fabs(atr->clips[0].start - 44100.0) < 1,
          "audio clip start restored (seconds<->samples)");
    CHECK(fabsf(atr->clips[0].clip_gain - 0.75f) < 0.01,
          "audio clip gain restored");
    CHECK(s->marker_count == 2, "markers restored (count)");
    CHECK(s->marker_count == 2 &&
          strcmp(s->markers[0].label, "Chapter One") == 0,
          "marker label survived");

    /* volume/pan on a track */
    vtr->volume = 0.11f;
    wb_shadowbin_write(s, path);
    wb_shadowbin_read(s, path);
    CHECK(fabsf(vtr->volume - 0.11f) < 0.01, "track volume round-trips");

    /* ---- bad input rejection ---- */
    FILE *bad = fopen("/tmp/wb_bad_bin.json", "w");
    if (bad) { fputs("}}}{ garbage", bad); fclose(bad); }
    int rc = wb_shadowbin_read(s, "/tmp/wb_bad_bin.json");
    CHECK(rc <= 0, "garbage JSON does not crash");
    CHECK(wb_shadowbin_read(s, "/tmp/nonexistent.json") == -1,
          "missing file -> -1");
    CHECK(wb_shadowbin_write(NULL, path) == -1, "NULL session rejected");

    free(buf);
    wb_session_destroy(s);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
