/* test_agent.c — headless agent API (R017 G9) + G1/G5/G8 cross-check.
 * Drives the editor via wb_agent_run over a FILE* script and verifies the
 * session model, EDL/FCPXML emission, and two-pass voice polish. */

#include "wbus/wbus_agent.h"
#include "wbus/wbus.h"
#include "wbus/wbus_video.h"
#include "wbus/wbus_compositor.h"
#include "wbus/wbus_voice_polish.h"
#include "wb_internal.h"   /* wb_wav_read/write_pcm16 (internal) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* access() for test-source probe */

static int checks = 0, failures = 0;
#define CK(cond, msg) do { checks++; if (!(cond)) { failures++; printf("  [FAIL] %s\n", msg); } else printf("  [PASS] %s\n", msg); } while (0)

static void write_script(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    fputs(body, f);
    fclose(f);
}

int main(void) {
    printf("=== agent API (G9) + G1/G5/G8 ===\n");

    /* Generate a real test source clip so import/split/EDL exercise real
     * decode + duration (ffmpeg full build). Skipped gracefully if absent. */
    system("/Users/waefrebeorn/.local/bin/ffmpeg -y -f lavfi -i "
           "testsrc=duration=4:size=320x240:rate=25 /tmp/agent_src.mp4 >/dev/null 2>&1");

    wb_engine *e = wb_engine_create();
    wb_session *s = wb_session_create();
    CK(e && s, "engine + session created");

    /* --- G9: run an agent script headlessly --- */
    int have_src = (access("/tmp/agent_src.mp4", R_OK) == 0);
    write_script("/tmp/agent_script.txt",
        "# import a clip on V1\n"
        "import /tmp/agent_src.mp4\n"
        "split 0 0 2.0\n"
        "quality 0.25\n"
        "edl /tmp/agent_out.edl\n"
        "fcpxml /tmp/agent_out.fcpxml\n");
    FILE *scr = fopen("/tmp/agent_script.txt", "r");
    CK(wb_agent_run(scr, s, e) == 0, "agent script ran with no errors");
    fclose(scr);

    /* verify the model changed: 1 video track, 2 clips after split */
    int vt = -1;
    for (uint32_t t = 0; t < s->track_count; t++)
        if (s->tracks[t].kind == WB_TRACK_KIND_VIDEO) { vt = (int)t; break; }
    if (have_src) {
        CK(vt >= 0, "agent import created a video track");
        CK(vt >= 0 && s->tracks[vt].clip_count == 2, "agent split produced 2 clips");
    } else {
        printf("  [skip] import checks (no test source clip)\n");
    }

    /* --- G1: quality dial is global and reflected in tile size --- */
    CK(wb_compositor_get_quality() > 0.2 && wb_compositor_get_quality() < 0.3,
       "agent 'quality 0.25' set the global QoS dial");
    int tile = wb_compositor_tile_size();
    CK(tile > 300 && tile < 400, "low quality -> smaller tile size (proxy draft)");
    wb_compositor_set_quality(1.0);
    CK(wb_compositor_tile_size() == 1024, "full quality -> full-res tile size");

    /* --- G5: EDL + FCPXML files written and well-formed --- */
    FILE *edl = fopen("/tmp/agent_out.edl", "r");
    CK(edl != NULL, "CMX3600 EDL file written (G5)");
    if (edl) {
        char line[256]; int ev_lines = 0;
        while (fgets(line, sizeof(line), edl))
            if (line[0] >= '0' && line[0] <= '9' && strstr(line, "V C")) ev_lines++;
        fclose(edl);
        if (have_src) CK(ev_lines == 2, "EDL has 2 clip events (one per split clip)");
        else CK(ev_lines == 0, "EDL empty when no source imported");
    }
    FILE *xml = fopen("/tmp/agent_out.fcpxml", "r");
    CK(xml != NULL, "FCPXML file written (G5)");
    if (xml) {
        char line[512]; int has_spine = 0, has_asset = 0;
        while (fgets(line, sizeof(line), xml)) {
            if (strstr(line, "<spine>")) has_spine++;
            if (strstr(line, "<asset ")) has_asset++;
        }
        fclose(xml);
        CK(has_spine == 1, "FCPXML has a <spine>");
        if (have_src) CK(has_asset == 2, "FCPXML has 2 <asset> refs (one per clip)");
        else CK(has_asset == 0, "FCPXML has 0 <asset> refs when no source");
    }

    /* --- G8: two-pass voice polish via agent 'polish' on a real WAV round-trip --- */
    /* synthesize a quiet 1s stereo tone, write it, polish, read back, measure */
    int sr = 44100, ch = 2; uint32_t n = (uint32_t)sr;
    float *buf = malloc((size_t)n * ch * sizeof(float));
    for (uint32_t i = 0; i < n * (uint32_t)ch; i++)
        buf[i] = 0.05f * (float)sinf(2.0f * 3.14159f * 220.0f * i / sr);
    CK(wb_wav_write_pcm16("/tmp/agent_in.wav", buf, n, ch, sr) == 0, "wrote test tone WAV");
    free(buf);

    write_script("/tmp/agent_polish.txt", "polish /tmp/agent_in.wav /tmp/agent_out.wav -16.0\n");
    scr = fopen("/tmp/agent_polish.txt", "r");
    CK(wb_agent_run(scr, s, e) == 0, "agent 'polish' (two-pass G8) ran");
    fclose(scr);

    float *out = NULL; uint32_t of = 0; int och = 0, osr = 0;
    CK(wb_wav_read_pcm16("/tmp/agent_out.wav", &out, &of, &och, &osr) == 0,
       "polished WAV read back");
    if (out) {
        float lufs = wb_loudness_measure(out, of, och, (float)osr);
        CK(lufs > -20.0f && lufs < -12.0f, "two-pass polish hit ~-16 LUFS target");
        /* peak must not clip */
        float pk = 0;
        for (uint32_t i = 0; i < of * (uint32_t)och; i++)
            if (fabsf(out[i]) > pk) pk = fabsf(out[i]);
        CK(pk <= 1.0001f, "polished output does not clip");
        free(out);
    }

    wb_session_destroy(s);
    wb_engine_destroy(e);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
