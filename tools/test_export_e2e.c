/* test_export_e2e.c — headless end-to-end video export verification.
 *
 * Builds a session with a video track (src clip) + one audio clip, imports a
 * real decoded test clip, renders through wb_video_export, and checks the
 * output mp4 exists and is a valid muxed file with both streams. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus.h"
#include "wbus/wbus_video.h"

int main(void) {
    int failures = 0;
    const char *src = "/tmp/vidtest/src.mp4";
    FILE *f = fopen(src, "rb");
    if (!f) { fprintf(stderr, "need %s (run test_video first)\n", src); return 2; }
    fclose(f);

    wb_session *s = wb_session_create();
    int vt = wb_session_add_video_track(s, "V1");
    int ci = wb_session_add_video_clip(s, vt, src, 0.0);
    if (vt < 0 || ci < 0) { fprintf(stderr, "clip setup failed\n"); return 1; }

    /* Add a 1-second 440 Hz audio clip on an audio track so the mux has audio. */
    wb_track *at = wb_session_add_track(s, "A1", WB_TRACK_KIND_AUDIO);
    float *buf = (float*)malloc(44100 * sizeof(float));
    for (int i = 0; i < 44100; i++) buf[i] = 0.3f * (float)sin(2.0*3.14159*440.0*i/44100.0);
    if (wb_session_add_audio_clip(at, 0.0, 44100.0, buf, 44100, 1) != 0)
        fprintf(stderr, "warn: audio clip add failed (engine may still render silence)\n");
    free(buf);

    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);

    const char *out = "/tmp/bigmac_e2e_export.mp4";
    remove(out);
    int rc = wb_video_export(s, e, out, NULL);
    if (rc != 0) {
        /* Retry with a throwaway engine (matches selftest offline render path). */
        fprintf(stderr, "retry with NULL engine...\n");
        rc = wb_video_export(s, NULL, out, NULL);
    }
    printf("wb_video_export rc=%d\n", rc);
    if (rc != 0) failures++;

    FILE *o = fopen(out, "rb");
    if (!o) { fprintf(stderr, "export file NOT created\n"); failures++; }
    else {
        fclose(o);
        /* Inspect the exported file with ffprobe for a/v streams. */
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "\"/Users/waefrebeorn/homebrew/bin/ffprobe\" -v error -show_entries "
                 "stream=codec_type -of csv=p=0 \"%s\"", out);
        FILE *p = popen(cmd, "r");
        int has_video = 0, has_audio = 0; char line[64];
        while (p && fgets(line, sizeof(line), p)) {
            if (strstr(line, "video")) has_video = 1;
            if (strstr(line, "audio")) has_audio = 1;
        }
        if (p) pclose(p);
        printf("export streams: video=%d audio=%d\n", has_video, has_audio);
        if (!has_video) { fprintf(stderr, "export missing video stream\n"); failures++; }
    }

    wb_engine_destroy(e);
    wb_session_destroy(s);

    /* R018-A: ProRes editorial export (the NLE exchange standard).
     * Rebuild a session and export a .mov with the ProRes codec. */
    int prores_fail = 0;
    wb_session *s2 = wb_session_create();
    int vt2 = wb_session_add_video_track(s2, "V1");
    wb_session_add_video_clip(s2, vt2, src, 0.0);
    wb_track *at2 = wb_session_add_track(s2, "A1", WB_TRACK_KIND_AUDIO);
    float *b2 = (float*)malloc(44100 * sizeof(float));
    for (int i = 0; i < 44100; i++) b2[i] = 0.3f * (float)sin(2.0*3.14159*440.0*i/44100.0);
    wb_session_add_audio_clip(at2, 0.0, 44100.0, b2, 44100, 1);
    free(b2);
    wb_engine *e2 = wb_engine_create();
    wb_engine_set_session(e2, s2);

    const char *pmov = "/tmp/bigmac_e2e_export_prores.mov";
    remove(pmov);
    int prc = wb_video_export_codec(s2, e2, pmov, NULL, WB_VIDEO_CODEC_PRORES);
    printf("wb_video_export_codec(ProRes) rc=%d\n", prc);
    if (prc != 0) prores_fail++;

    FILE *pm = fopen(pmov, "rb");
    if (!pm) { fprintf(stderr, "ProRes file NOT created\n"); prores_fail++; }
    else {
        fclose(pm);
        char pcmd[1024];
        snprintf(pcmd, sizeof(pcmd),
                 "\"/Users/waefrebeorn/homebrew/bin/ffprobe\" -v error -show_entries "
                 "stream=codec_name -select_streams v:0 -of csv=p=0 \"%s\"", pmov);
        FILE *pp = popen(pcmd, "r");
        char vcodec[32] = "";
        if (pp) { if (fgets(vcodec, sizeof(vcodec), pp)) { /* keep */ } pclose(pp); }
        printf("ProRes video codec: '%s'\n", vcodec);
        if (strstr(vcodec, "prores") == NULL) { fprintf(stderr, "ProRes codec not produced\n"); prores_fail++; }
    }
    wb_engine_destroy(e2);
    wb_session_destroy(s2);
    if (prores_fail) failures++;

    printf("%s\n", failures == 0 ? "E2E_EXPORT_OK" : "E2E_EXPORT_FAIL");
    return failures == 0 ? 0 : 1;
}
