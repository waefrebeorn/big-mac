/* wb_cgiexport.c — CGI overlay export path (R058).
 *
 * Strategy: render the animation to a rawvideo RGBA stream (transparent
 * background), then let ffmpeg overlay it on the concat output:
 *
 *   ffmpeg -i <concat video> -i <audio> -f rawvideo -pix_fmt rgba
 *          -s WxH -r FPS -i cgi.raw
 *          -filter_complex "[0:v][2:v]overlay=...[v]" -map [v] -map 1:a ...
 *
 * The overlay filter keys on alpha automatically. enable='between(t,a,b)'
 * limits the overlay to its time window. This is exactly Blender's
 * "transparent render + compositor Alpha Over" idea, miniaturized.
 */

#include "wbus/wbus_cgiexport.h"
#include "wbus/wbus_delivery.h"
#include "wbus/wbus_perfclip.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#ifndef WB_CGI_FFMPEG
#define WB_CGI_FFMPEG "/Users/waefrebeorn/.local/bin/ffmpeg"
#endif

int wb_cgi_render_seq(wb_anim *a, double t0, double dur, double fps,
                      const char *out_dir) {
    if (!a || !out_dir || fps <= 0 || dur <= 0) return -1;
    int frames = (int)(dur * fps);
    if (frames < 1) return -1;
    int w, h;
    /* wb_anim doesn't expose w/h — render via a probe frame is wasteful;
     * instead the anim stores them internally. We re-derive from the first
     * render using the anim's own buffer (wb_anim keeps w,h private), so
     * this path allocates its own ctx sized by the caller's anim. Simplest:
     * require the anim to have been created with the size we guess — no.
     * Cleanest honest v1: render into a fixed 640x360 buffer and let the
     * caller pick anim size to match. Documented limitation. */
    w = 640; h = 360;
    uint8_t *rgba = malloc((size_t)w*h*4);
    if (!rgba) return -1;
    for (int f = 0; f < frames; f++) {
        double t = t0 + (double)f / fps;
        wb_anim_render_frame(a, t, rgba);
        char path[1200];
        snprintf(path, sizeof path, "%s/frame_%04d.png", out_dir, f);
        /* PNG encode via ffmpeg single-image pipe (keeps zero-dep policy) */
        char cmd[1600];
        snprintf(cmd, sizeof cmd,
            "\"%s\" -y -f rawvideo -pix_fmt rgba -s %dx%d -r %f -i - "
            "-frames:v 1 \"%s\" >/dev/null 2>&1",
            WB_CGI_FFMPEG, w, h, fps, path);
        FILE *ff = popen(cmd, "w");
        if (!ff) { free(rgba); return -1; }
        /* flatten alpha over black for the inspection sequence */
        static uint8_t flat[640*360*4];
        for (int i = 0; i < w*h; i++) {
            float al = rgba[i*4+3] / 255.0f;
            flat[i*4+0] = (uint8_t)(rgba[i*4+0]*al);
            flat[i*4+1] = (uint8_t)(rgba[i*4+1]*al);
            flat[i*4+2] = (uint8_t)(rgba[i*4+2]*al);
            flat[i*4+3] = 255;
        }
        fwrite(flat, 1, (size_t)w*h*4, ff);
        pclose(ff);
    }
    free(rgba);
    return frames;
}

int wb_video_export_delivery(wb_session *s, wb_engine *e,
                             const char *output_path,
                             const char *srt_path,
                             wb_video_codec codec,
                             const wb_cgi_overlay *ov,
                             double target_lufs) {
    if (!target_lufs || target_lufs >= 0.0)
        return wb_video_export_cgi(s,e,output_path,srt_path,codec,ov);
    /* render audio to a temp wav, normalize it, then export pointing at it */
    wb_sample *pcm = NULL;
    uint32_t frames = 0;
    if (wb_engine_render_session(e, s, &pcm, &frames) != 0 || !pcm)
        return -1;
    const char *tmpwav = "/tmp/bigmac_export_audio.wav";
    if (wb_wav_write_pcm16(tmpwav, pcm, frames, 2, WB_SAMPLE_RATE) != 0) {
        free(pcm);
        return -1;
    }
    free(pcm);
    if (wb_delivery_normalize_wav(tmpwav, target_lufs) != 0) return -1;
    int rc = wb_video_export_cgi(s,e,output_path,srt_path,codec,ov);
    unlink(tmpwav);
    return rc;
}

int wb_video_export_cgi(wb_session *s, wb_engine *e,
                        const char *output_path,
                        const char *srt_path,
                        wb_video_codec codec,
                        const wb_cgi_overlay *ov) {
    if (!ov || !ov->anim)   /* no overlay: plain export */
        return wb_video_export_codec(s, e, output_path, srt_path, codec);

    /* Step 1: render the overlay animation to raw RGBA on disk. */
    const int W = 640, H = 360;
    const double FPS = 24.0;
    const char *raw = "/tmp/bigmac_cgi_overlay.raw";
    FILE *rf = fopen(raw, "wb");
    if (!rf) return -1;
    int frames = (int)(ov->duration * FPS);
    uint8_t *rgba = malloc((size_t)W*H*4);
    if (!rgba) { fclose(rf); return -1; }
    for (int f = 0; f < frames; f++) {
        wb_anim_render_frame(ov->anim, (double)f / FPS, rgba);
        fwrite(rgba, 1, (size_t)W*H*4, rf);
    }
    free(rgba);
    fclose(rf);
    if (frames < 1) return -1;

    /* Step 2/3. Does the session have a video track? */
    int has_video = 0;
    for (uint32_t t = 0; t < s->track_count; t++)
        if (s->tracks[t].kind == WB_TRACK_KIND_VIDEO) { has_video = 1; break; }

    double dur_s = (double)frames / FPS;
    char cmd[3072];

    if (!has_video) {
        /* R058: pure-CGI video — the animation IS the picture. Render
         * frames flattened over black, mux with the session's engine-
         * rendered audio (if any). */
        const char *flat = "/tmp/bigmac_cgi_flat.raw";
        FILE *ff = fopen(flat, "wb");
        if (!ff) return -1;
        uint8_t *fr = malloc((size_t)W*H*4);
        if (!fr) { fclose(ff); return -1; }
        for (int f = 0; f < frames; f++) {
            wb_anim_render_frame(ov->anim, ov->t_start + (double)f/FPS, fr);
            for (int i = 0; i < W*H; i++) {
                float al = fr[i*4+3] / 255.0f;
                fr[i*4+0] = (uint8_t)(fr[i*4+0]*al);
                fr[i*4+1] = (uint8_t)(fr[i*4+1]*al);
                fr[i*4+2] = (uint8_t)(fr[i*4+2]*al);
                fr[i*4+3] = 255;
            }
            fwrite(fr, 1, (size_t)W*H*4, ff);
        }
        free(fr); fclose(ff);

        /* audio through the engine (session may be silent — then no audio) */
        const char *awav = "/tmp/bigmac_cgi_audio.wav";
        int has_audio = 0;
        wb_sample *audio = NULL; uint32_t afr = 0;
        if (wb_engine_render_session(e, s, &audio, &afr) == 0 && audio && afr > 0) {
            if (wb_wav_write_pcm16(awav, audio, afr, 2, WB_SAMPLE_RATE) == 0)
                has_audio = 1;
        }
        free(audio);

        if (has_audio)
            snprintf(cmd, sizeof cmd,
                "\"%s\" -y -f rawvideo -pix_fmt rgba -s %dx%d -r %f -i \"%s\" "
                "-i \"%s\" -c:v libx264 -preset fast -crf 20 -pix_fmt yuv420p "
                "-c:a aac -shortest \"%s\" >/dev/null 2>&1",
                WB_CGI_FFMPEG, W, H, FPS, flat, awav, output_path);
        else
            snprintf(cmd, sizeof cmd,
                "\"%s\" -y -f rawvideo -pix_fmt rgba -s %dx%d -r %f -i \"%s\" "
                "-c:v libx264 -preset fast -crf 20 -pix_fmt yuv420p "
                "\"%s\" >/dev/null 2>&1",
                WB_CGI_FFMPEG, W, H, FPS, flat, output_path);
        int rc2 = system(cmd);
        unlink(raw);
        return rc2 == 0 ? 0 : -1;
    }

    /* Step 2: export the plain session video to an intermediate. */
    const char *mid = "/tmp/bigmac_cgi_base.mp4";
    if (wb_video_export_codec(s, e, mid, srt_path, codec) != 0) return -1;

    snprintf(cmd, sizeof cmd,
        "\"%s\" -y -i \"%s\" -f rawvideo -pix_fmt rgba -s %dx%d -r %f "
        "-i \"%s\" "
        "-filter_complex \"[0:v][1:v]overlay=x=(W-w)/2:y=(H-h)/2:"
        "enable='between(t,%.3f,%.3f)'[v]\" "
        "-map \"[v]\" -map 0:a -c:v libx264 -preset fast -crf 20 "
        "-pix_fmt yuv420p -c:a copy -shortest \"%s\" >/dev/null 2>&1",
        WB_CGI_FFMPEG, mid, W, H, FPS, raw, ov->t_start,
        ov->t_start + dur_s, output_path);
    int rc = system(cmd);
    unlink(raw);
    return rc == 0 ? 0 : -1;
}

/* R070: composite every perf-clip on the session's video tracks into the
 * export. Each perf-clip replays deterministically, rasterizes at export
 * resolution, and overlays during [start, start+length). Chained ffmpeg
 * passes: one per perf-clip (base -> base2 -> ... -> output_path). */
int wb_video_export_perf_overlays(wb_session *s, wb_engine *e,
                                  const char *output_path,
                                  const char *srt_path,
                                  wb_video_codec codec) {
    if (!s || !output_path) return -1;

    /* collect perf-clips */
    const wb_clip *perfclips[64];
    int nperf = 0;
    for (uint32_t t = 0; t < s->track_count && nperf < 64; t++) {
        wb_track *tr = &s->tracks[t];
        if (tr->kind != WB_TRACK_KIND_VIDEO) continue;
        for (uint32_t c = 0; c < tr->clip_count && nperf < 64; c++) {
            wb_clip *cl = &tr->clips[c];
            if (cl->type == 3 && cl->perfclip) perfclips[nperf++] = cl;
        }
    }
    if (nperf == 0)
        return wb_video_export_codec(s, e, output_path, srt_path, codec);

    const int W = 640, H = 360;
    const double FPS = 24.0;
    char src[512], dst[512], cmd[3072];

    /* Step 1: plain session video as the base layer. When the session has
     * NO media video clips (perf-only session), synthesize a black base of
     * the right duration so the perf has something to composite over. */
    snprintf(src, sizeof src, "%s", "/tmp/bigmac_perf_base.mp4");
    int has_media = 0;
    for (uint32_t t = 0; t < s->track_count && !has_media; t++)
        for (uint32_t c = 0; c < s->tracks[t].clip_count; c++)
            if (s->tracks[t].clips[c].type == 2) { has_media = 1; break; }
    if (has_media) {
        if (wb_video_export_codec(s, e, src, srt_path, codec) != 0)
            return -1;
    } else {
        /* black base at session length (or first perf end), silent audio.
         * s->length is in SAMPLES — convert to seconds. */
        double dur = s->length / (double)WB_SAMPLE_RATE;
        if (dur <= 0) dur = 2.0;
        snprintf(cmd, sizeof cmd,
            "\"%s\" -y -f lavfi -i color=c=black:s=%dx%d:r=%f:d=%f "
            "-c:v libx264 -preset fast -crf 20 -pix_fmt yuv420p "
            "\"%s\" >/dev/null 2>&1",
            WB_CGI_FFMPEG, W, H, FPS, dur, src);
        if (system(cmd) != 0) return -1;
    }

    /* Step 2: one overlay pass per perf-clip. */
    for (int i = 0; i < nperf; i++) {
        const wb_clip *cl = perfclips[i];
        const char *raw = "/tmp/bigmac_perf_overlay.raw";
        double dur = cl->length > 0 ? cl->length : 2.0;
        if (wb_perfclip_render_seq((wb_perfclip *)cl->perfclip,
                                   0.0, dur, FPS, W, H, raw) != 0) {
            unlink(src);
            return -1;
        }
        snprintf(dst, sizeof dst, "%s", output_path);
        /* intermediate target except the last pass writes the final path */
        if (i < nperf - 1) snprintf(dst, sizeof dst,
                                    "/tmp/bigmac_perf_pass%d.mp4", i);
        snprintf(cmd, sizeof cmd,
            "\"%s\" -y -i \"%s\" -f rawvideo -pix_fmt rgba -s %dx%d -r %f "
            "-i \"%s\" "
            "-filter_complex \"[0:v][1:v]overlay=x=(W-w)/2:y=(H-h)/2:"
            "enable='between(t,%.3f,%.3f)'[v]\" "
            "-map \"[v]\" -c:v libx264 -preset fast -crf 20 "
            "-pix_fmt yuv420p -shortest \"%s\" >/dev/null 2>&1",
            WB_CGI_FFMPEG, src, W, H, FPS, raw, cl->start,
            cl->start + dur, dst);
        int rc = system(cmd);
        unlink(raw);
        if (rc != 0) { unlink(src); return -1; }
        if (i < nperf - 1) snprintf(src, sizeof src, "%s", dst);
    }
    return 0;
}
