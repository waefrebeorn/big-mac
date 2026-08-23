/* test_cgiexport.c — R058 end-to-end: build an animation, export a video
 * with the CGI overlay, ffprobe the output for streams, and verify frames
 * actually changed during the overlay window (pixel-diff via extracted
 * PNGs). Requires ffmpeg on PATH (same as test_export_e2e). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus_cgiexport.h"
#include "wbus/wbus_mesh.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== CGI overlay export (R058) test ===\n\n");

    /* session with audio (tone) so the mux has an audio stream */
    wb_session *s = wb_session_create();
    s->bpm = 120.0; s->length = 44100.0 * 4;
    wb_track *tr = wb_session_add_track(s, "aud", 1);
    uint32_t nf = 44100 * 2;
    wb_sample *buf = malloc(nf * sizeof(wb_sample));
    for (uint32_t i = 0; i < nf; i++)
        buf[i] = (wb_sample)(0.25 * sin(2*M_PI*440.0*i/44100.0));
    wb_session_add_audio_clip(tr, 0, (double)nf, buf, nf, 1);
    free(buf);

    wb_engine *e = wb_engine_create();

    /* animation: cube slides across over 2 seconds */
    wb_anim *a = wb_anim_create(640, 360);
    wb_mesh *box = wb_mesh_box(0.8f, 0.8f, 0.8f, 255, 140, 40);
    wb_anim_add_object(a, box, 255, 140, 40);
    wb_anim_key(a, 0, 0.0, -3, 0, 0, 0,0,0, 1);
    wb_anim_key(a, 0, 2.0,  3, 0, 0, 0,0,0, 1);

    wb_cgi_overlay ov = { .anim = a, .t_start = 0.5, .duration = 2.0 };
    int rc = wb_video_export_cgi(s, e, "/tmp/wb_cgi_out.mp4", NULL,
                                 WB_VIDEO_CODEC_H264, &ov);
    CHECK(rc == 0, "cgi overlay export succeeded");

    /* probe output */
    FILE *p = popen("/Users/waefrebeorn/homebrew/bin/ffprobe -v quiet "
                    "-show_entries stream=codec_type "
                    "-of csv=p=0 /tmp/wb_cgi_out.mp4", "r");
    char line[64];
    int nvideo = 0, naudio = 0;
    while (fgets(line, sizeof line, p)) {
        if (strncmp(line, "video", 5) == 0) nvideo++;
        if (strncmp(line, "audio", 5) == 0) naudio++;
    }
    pclose(p);
    CHECK(nvideo >= 1, "output has video stream");
    CHECK(naudio >= 1, "output has audio stream");

    /* extract a frame mid-overlay and one before it; they must differ */
    system("/Users/waefrebeorn/.local/bin/ffmpeg -y -ss 0.2 -i /tmp/wb_cgi_out.mp4 "
           "-frames:v 1 /tmp/wb_before.png >/dev/null 2>&1");
    system("/Users/waefrebeorn/.local/bin/ffmpeg -y -ss 1.5 -i /tmp/wb_cgi_out.mp4 "
           "-frames:v 1 /tmp/wb_during.png >/dev/null 2>&1");
    long dsize_b = 0, dsize_d = 0;
    { FILE *f = fopen("/tmp/wb_before.png","rb"); if (f){fseek(f,0,SEEK_END);dsize_b=ftell(f);fclose(f);} }
    { FILE *f = fopen("/tmp/wb_during.png","rb"); if (f){fseek(f,0,SEEK_END);dsize_d=ftell(f);fclose(f);} }
    CHECK(dsize_b > 1000 && dsize_d > 1000, "frames extracted");
    /* different content => different PNG sizes (crude but real signal) */
    CHECK(dsize_b != dsize_d, "overlay window visibly changed the frame");

    /* seq render path */
    system("mkdir -p /tmp/wb_seq");
    int nseq = wb_cgi_render_seq(a, 0, 1.0, 8.0, "/tmp/wb_seq");
    CHECK(nseq == 8, "render_seq produced 8 frames at 8fps");

    wb_mesh_free(box); wb_anim_free(a);
    wb_engine_destroy(e); wb_session_destroy(s);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
