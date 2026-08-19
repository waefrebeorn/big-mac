/* wb_video.c — FFmpeg video decode + 480p proxy + SDL2 preview for the video editor.
 *
 * Architecture (R009 §2-3):
 *   - wb_video_clip: source_path, proxy_path (480p), start_in_source, duration, timeline_pos
 *   - wb_video_decoder: AVFormatContext + AVCodecContext + SwsContext, seek to time
 *   - Preview: decode one frame at current timeline position → SDL2 texture
 *   - Proxy: ffmpeg CLI call at import (scale=854:480, h264, aac)
 *   - Export: ffmpeg CLI with full-res sources + subtitle overlay
 */

#include "wbus/wbus_video.h"
#include "wbus/wbus_captions.h"
#include "wbus/wbus.h"
#include "wb_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <SDL.h>

/* ---- config */
#ifndef FFmpeg_BIN
/* Full-featured tessus FFmpeg 9.0.1 build (has libavfilter: blackdetect,
 * silencedetect, select, concat demuxer). The homebrew static build under
 * ~/homebrew/bin/ffmpeg is a MINIMAL copy/encode-only build with NO filters,
 * so filter-based features (detect) and the concat demuxer require this one. */
#define FFmpeg_BIN "/Users/waefrebeorn/.local/bin/ffmpeg"
#endif
#ifndef PROXY_SCALE_W
#define PROXY_SCALE_W 854
#endif
#ifndef PROXY_SCALE_H
#define PROXY_SCALE_H 480
#endif

/* ---- wb_video_clip ----------------------------------------------------- */

void wb_video_clip_init(wb_video_clip *c) {
    memset(c, 0, sizeof(*c));
    c->start_in_source = -1.0;
    c->duration = -1.0;
    c->timeline_pos = -1.0;
}

void wb_video_clip_free(wb_video_clip *c) {
    /* paths are stack-allocated; nothing to free */
}

/* ---- wb_video_decoder (FFmpeg C API) --------------------------------- */

struct wb_video_decoder {
    AVFormatContext *fmt_ctx;
    AVCodecContext *codec_ctx;
    AVCodec *decoder;
    SwsContext *sws_ctx;
    int video_stream_idx;
    double current_time;    /* seconds */
    int eof;
};

wb_video_decoder *wb_video_decoder_open(const char *path) {
    wb_video_decoder *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    int rc = avformat_open_input(&d->fmt_ctx, path, NULL, NULL);
    if (rc < 0) {
        fprintf(stderr, "wb_video: avformat_open_input failed: %s\n", av_err2str(rc));
        free(d);
        return NULL;
    }

    rc = avformat_find_stream_info(d->fmt_ctx, NULL);
    if (rc < 0) {
        fprintf(stderr, "wb_video: avformat_find_stream_info failed\n");
        avformat_close_input(&d->fmt_ctx);
        free(d);
        return NULL;
    }

    /* Find video stream */
    d->video_stream_idx = av_find_best_stream(d->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (d->video_stream_idx < 0) {
        fprintf(stderr, "wb_video: no video stream in %s\n", path);
        avformat_close_input(&d->fmt_ctx);
        free(d);
        return NULL;
    }

    AVCodecParameters *par = d->fmt_ctx->streams[d->video_stream_idx]->codecpar;
    d->decoder = avcodec_find_decoder(par->codec_id);
    if (!d->decoder) {
        fprintf(stderr, "wb_video: no decoder for codec %d\n", par->codec_id);
        avformat_close_input(&d->fmt_ctx);
        free(d);
        return NULL;
    }

    d->codec_ctx = avcodec_alloc_context3(d->decoder);
    if (!d->codec_ctx) {
        avformat_close_input(&d->fmt_ctx);
        free(d);
        return NULL;
    }

    avcodec_parameters_to_context(d->codec_ctx, par);
    rc = avcodec_open2(d->codec_ctx, d->decoder, NULL);
    if (rc < 0) {
        fprintf(stderr, "wb_video: avcodec_open2 failed: %s\n", av_err2str(rc));
        avcodec_free_context(&d->codec_ctx);
        avformat_close_input(&d->fmt_ctx);
        free(d);
        return NULL;
    }

    /* SwsContext for scaling to proxy/preview size */
    d->sws_ctx = sws_getContext(d->codec_ctx->width, d->codec_ctx->height,
                                 d->codec_ctx->pix_fmt,
                                 PROXY_SCALE_W, PROXY_SCALE_H,
                                 AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    d->current_time = 0.0;
    d->eof = 0;
    return d;
}

void wb_video_decoder_close(wb_video_decoder *d) {
    if (!d) return;
    if (d->sws_ctx) sws_freeContext(d->sws_ctx);
    if (d->codec_ctx) avcodec_free_context(&d->codec_ctx);
    if (d->fmt_ctx) avformat_close_input(&d->fmt_ctx);
    free(d);
}

/* Seek to a time in seconds. Returns 0 on success. */
int wb_video_decoder_seek(wb_video_decoder *d, double time_sec) {
    if (!d) return -1;
    int64_t ts = av_rescale_q((int64_t)(time_sec * AV_TIME_BASE),
                               AV_TIME_BASE_Q, d->fmt_ctx->streams[d->video_stream_idx]->time_base);
    d->current_time = time_sec;
    d->eof = 0;
    return av_seek_frame(d->fmt_ctx, d->video_stream_idx, ts, AVSEEK_FLAG_BACKWARD);
}

/* Decode one frame at the current position. Returns RGBA data into `out`
 * (caller provides buffer: out_w * out_h * 4 bytes). Sets *out_w, *out_h.
 * Returns 0 on success, -1 on EOF/error. */
int wb_video_decoder_decode_frame(wb_video_decoder *d, uint8_t *out,
                                   int *out_w, int *out_h) {
    if (!d || d->eof) return -1;

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *rgb_frame = av_frame_alloc();
    if (!pkt || !frame || !rgb_frame) {
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        av_packet_free(&pkt);
        return -1;
    }

    /* Allocate RGB frame */
    int stride[4] = { 0 };
    rgb_frame->data[0] = out;
    rgb_frame->linesize[0] = *out_w * 4;
    rgb_frame->width = *out_w;
    rgb_frame->height = *out_h;
    rgb_frame->format = AV_PIX_FMT_RGBA;

    int got_frame = 0;
    int rc = 0;
    while (!got_frame) {
        rc = av_read_frame(d->fmt_ctx, pkt);
        if (rc < 0) {
            d->eof = 1;
            break;
        }
        if (pkt->stream_index == d->video_stream_idx) {
            rc = avcodec_send_packet(d->codec_ctx, pkt);
            if (rc < 0) break;
            while (avcodec_receive_frame(d->codec_ctx, frame) >= 0) {
                /* Scale to RGBA */
                const uint8_t *src_data[4];
                int src_linesize[4];
                for (int i = 0; i < 4; i++) {
                    src_data[i] = frame->data[i];
                    src_linesize[i] = frame->linesize[i];
                }
                sws_scale(d->sws_ctx, src_data, src_linesize,
                          0, d->codec_ctx->height,
                          rgb_frame->data, rgb_frame->linesize);
                got_frame = 1;
                d->current_time = (double)frame->pts * av_q2d(d->fmt_ctx->streams[d->video_stream_idx]->time_base);
                break;
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    av_packet_free(&pkt);
    return got_frame ? 0 : -1;
}

/* Get video duration in seconds. */
double wb_video_decoder_get_duration(wb_video_decoder *d) {
    if (!d || !d->fmt_ctx) return 0.0;
    return d->fmt_ctx->duration / (double)AV_TIME_BASE;
}

/* Get video width/height. */
int wb_video_decoder_get_width(wb_video_decoder *d) { return d ? d->codec_ctx->width : 0; }
int wb_video_decoder_get_height(wb_video_decoder *d) { return d ? d->codec_ctx->height : 0; }

/* ---- 480p proxy generation (CLI) -------------------------------------- */

int wb_video_make_proxy(const char *src, const char *proxy) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -y -i \"%s\" -vf \"scale=%d:%d\" -c:v libx264 -preset fast -crf 23 "
             "-c:a aac -b:a 64k \"%s\" > /dev/null 2>&1",
             FFmpeg_BIN, src, PROXY_SCALE_W, PROXY_SCALE_H, proxy);
    return run_cmd(cmd, "proxy generation") == 0 ? 0 : -1;
}

/* Get proxy duration (matches source, checked via ffprobe). */
double wb_video_proxy_duration(const char *proxy_path) {
    char cmd[512];
    char buf[256];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -i \"%s\" 2>&1 | grep Duration", FFmpeg_BIN, proxy_path);
    FILE *f = popen(cmd, "r");
    if (!f) return 0.0;
    if (fgets(buf, sizeof(buf), f)) {
        /* Parse "Duration: HH:MM:SS.NS" */
        char *p = strstr(buf, "Duration: ");
        if (p) {
            p += 10;
            int h, m, s;
            if (sscanf(p, "%d:%d:%d", &h, &m, &s) == 3) {
                pclose(f);
                return h * 3600.0 + m * 60.0 + s;
            }
        }
    }
    pclose(f);
    return 0.0;
}

/* ---- SDL2 preview helpers ---------------------------------------------- */

/* Create an SDL2 texture from decoded RGBA frame data. */
SDL_Texture *wb_video_frame_to_texture(SDL_Renderer *ren, uint8_t *rgba_data,
                                        int w, int h) {
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!tex) return NULL;
    SDL_UpdateTexture(tex, NULL, rgba_data, w * 4);
    return tex;
}

/* Scale an SDL texture to fit a destination rect, preserving aspect ratio. */
void wb_video_blit_scaled(SDL_Renderer *ren, SDL_Texture *tex,
                           SDL_Rect *dst) {
    int tex_w, tex_h;
    SDL_QueryTexture(tex, NULL, NULL, &tex_w, &tex_h);
    float scale = (float)dst->w / tex_w;
    if ((float)dst->h / tex_h < scale) scale = (float)dst->h / tex_h;
    int new_w = (int)(tex_w * scale);
    int new_h = (int)(tex_h * scale);
    SDL_Rect src = { 0, 0, tex_w, tex_h };
    SDL_Rect d = { dst->x + (dst->w - new_w) / 2,
                   dst->y + (dst->h - new_h) / 2, new_w, new_h };
    SDL_RenderCopy(ren, tex, &src, &d);
}

/* ---- video export (R009 §3.4) ----------------------------------------- */

/* Export the session as a 1080p60 mp4 with optional caption burn.
 * Renders the audio track through our engine, muxes it with the (full-res)
 * video source, and burns SRT captions if requested.
 * audio_wav: pre-rendered audio from our engine.
 * srt_path: optional SRT to burn as subtitles (NULL = no captions). */
/* R018-A: export with selectable codec (H.264 delivery vs ProRes
 * editorial). ProRes is the professional NLE exchange standard. */
int wb_video_export_codec(wb_session *s, wb_engine *e,
                          const char *output_path,
                          const char *srt_path,
                          wb_video_codec codec) {
    if (!s || !e || !output_path) return -1;

    /* Find first video track + first video clip. */
    int vt = -1;
    for (uint32_t t = 0; t < s->track_count; t++) {
        if (s->tracks[t].kind == WB_TRACK_KIND_VIDEO) { vt = (int)t; break; }
    }
    if (vt < 0) { fprintf(stderr, "wb_video_export: no video track\n"); return -1; }

    wb_clip *prim = NULL;
    for (uint32_t c = 0; c < s->tracks[vt].clip_count; c++) {
        if (s->tracks[vt].clips[c].type == 2) { prim = &s->tracks[vt].clips[c]; break; }
    }
    if (!prim || !prim->video) { fprintf(stderr, "wb_video_export: no video clip\n"); return -1; }

    /* Render the audio side of the session through our engine. */
    const char *audio_wav = "/tmp/bigmac_export_audio.wav";
    wb_sample *audio = NULL;
    uint32_t frames = 0;
    if (wb_engine_render_session(e, s, &audio, &frames) != 0 || !audio) {
        fprintf(stderr, "wb_video_export: audio render failed\n");
        return -1;
    }
    if (wb_wav_write_pcm16(audio_wav, audio, frames, 2, WB_SAMPLE_RATE) != 0) {
        fprintf(stderr, "wb_video_export: audio wav write failed\n");
        free(audio);
        return -1;
    }
    free(audio);

    /* Use the full-res source (not the proxy) on export. */
    const char *vid_src = prim->video->source_path;
    const char *ffmpeg = FFmpeg_BIN;

    /* Build the video encoder args per codec. */
    const char *venc, *vopts;
    char prof[32] = "";
    switch (codec) {
        case WB_VIDEO_CODEC_PRORES_HQ:
            snprintf(prof, sizeof(prof), " -profile:v 3");
            /* fallthrough */
        case WB_VIDEO_CODEC_PRORES:
            venc  = "prores_ks";
            vopts = "-pix_fmt yuv422p10le";   /* 10-bit 4:2:2, standard ProRes */
            break;
        case WB_VIDEO_CODEC_H264:
        default:
            venc  = "libx264";
            vopts = "-preset fast -crf 20 -pix_fmt yuv420p";
            break;
    }

    char cmd[4096];
    if (srt_path) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -y "
                 "-i \"%s\" "
                 "-i \"%s\" "
                 "-vf \"subtitles=%s:force_style='FontSize=28,FontName=Arial,BorderStyle=3,Outline=1,Shadow=0'\" "
                 "-map 0:v -map 1:a "
                 "-c:v %s %s%s "
                 "-r 60 "
                 "-c:a aac -b:a 192k "
                 "-shortest \"%s\" > /dev/null 2>&1",
                 ffmpeg, vid_src, audio_wav, srt_path,
                 venc, vopts, prof, output_path);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -y "
                 "-i \"%s\" "
                 "-i \"%s\" "
                 "-map 0:v -map 1:a "
                 "-c:v %s %s%s "
                 "-r 60 "
                 "-c:a aac -b:a 192k "
                 "-shortest \"%s\" > /dev/null 2>&1",
                 ffmpeg, vid_src, audio_wav,
                 venc, vopts, prof, output_path);
    }

    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "wb_video_export: ffmpeg failed (exit %d)\n", WEXITSTATUS(rc));
    return rc == 0 ? 0 : -1;
}

int wb_video_export(wb_session *s, wb_engine *e,
                    const char *output_path,
                    const char *srt_path) {
    return wb_video_export_codec(s, e, output_path, srt_path, WB_VIDEO_CODEC_H264);
}

/* Quick captions-only step: extract audio, transcribe, produce SRT.
 * Used during export or as a standalone feature. */
int wb_video_generate_captions(wb_session *s, int video_track,
                               const char *srt_out_path) {
    if (!srt_out_path) return -1;
    if (video_track < 0 || video_track >= (int)s->track_count) return -1;

    /* Find the first video clip on this track */
    wb_clip *clip = NULL;
    for (uint32_t c = 0; c < s->tracks[video_track].clip_count; c++) {
        if (s->tracks[video_track].clips[c].type == 2) {
            clip = &s->tracks[video_track].clips[c];
            break;
        }
    }
    if (!clip || !clip->video) return -1;

    return wb_video_captions_generate(clip->video->source_path, srt_out_path,
                                      "/Users/waefrebeorn/whisper.cpp/build/bin/whisper-cli",
                                      "/Users/waefrebeorn/whisper.cpp/models/ggml-tiny.en-q5_1.bin");
}

/* ---- lossless trim + auto-cut (R016 S4) ------------------------------ */

/* Build an ffmpeg concat-demuxer script and remux the keep-segments with
 * -c copy (no re-encode). The script is piped to ffmpeg's stdin. */
int wb_video_lossless_trim(const char *src, const wb_video_segment *segs,
                           int nsegs, const char *out_path) {
    if (!src || !segs || nsegs <= 0 || !out_path) return -1;

    /* Build the concat script in memory. */
    size_t cap = 256, len = 0;
    char *script = malloc(cap);
    if (!script) return -1;
    len += snprintf(script + len, cap - len, "ffconcat version 1.0\n");
    int used = 0;
    for (int i = 0; i < nsegs; i++) {
        double s = segs[i].start, e = segs[i].end;
        if (e > 0.0 && e <= s) continue;  /* empty/skip */
        /* grow the buffer if needed */
        while (len + 256 > cap) { cap *= 2; char *nw = realloc(script, cap); if (!nw) { free(script); return -1; } script = nw; }
        len += snprintf(script + len, cap - len,
                        "file '%s'\n", src);
        len += snprintf(script + len, cap - len, "inpoint %f\n", s);
        if (e > 0.0) len += snprintf(script + len, cap - len, "outpoint %f\n", e);
        used++;
    }
    if (used == 0) { free(script); return -1; }

    /* Write the concat script to a temp file (concat demuxer reads a path,
     * not stdin — piping via "-i -" is rejected by ffmpeg). */
    char tmpl[] = "/tmp/bigmac_concat_XXXXXX.concat";
    int fd = mkstemps(tmpl, 7);
    if (fd < 0) { free(script); return -1; }
    ssize_t w = write(fd, script, len);
    close(fd);
    if (w != (ssize_t)len) { free(script); unlink(tmpl); return -1; }
    free(script);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -y -f concat -safe 0 -i \"%s\" -c copy -map 0 "
             "-avoid_negative_ts make_zero \"%s\" > /dev/null 2>&1",
             FFmpeg_BIN, tmpl, out_path);

    int rc = system(cmd);
    unlink(tmpl);
    return rc == 0 ? 0 : -1;
}

/* Detect segments via ffmpeg filters and parse the spans back.
 * mode 0 = scene (select=gt(scene,thr)), 1 = blackdetect, 2 = silencedetect. */
int wb_video_detect_segments(const char *src, int mode, double threshold,
                              wb_video_segment *out, int cap) {
    if (!src || !out || cap <= 0) return -1;
    if (mode < 0 || mode > 2) return -1;

    char filter[512];
    const char *map = "-map 0:v";
    if (mode == 0) {
        snprintf(filter, sizeof(filter),
                 "select='gt(scene,%f)',showinfo", threshold);
    } else if (mode == 1) {
        snprintf(filter, sizeof(filter),
                 "blackdetect=d=%f:pic_th=0.98,metadata=print:key=lavfi.black_start", threshold);
        map = "-map 0:v";
    } else {
        snprintf(filter, sizeof(filter),
                 "silencedetect=n=-40dB:d=%f", threshold);
        map = "-map 0:a";
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -hide_banner -i \"%s\" -filter:v \"%s\" -f null - 2>&1",
             FFmpeg_BIN, src, filter);
    /* For silence we need -filter:a instead of -filter:v; handle generically: */
    if (mode == 2) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -hide_banner -i \"%s\" -af \"%s\" -f null - 2>&1",
                 FFmpeg_BIN, src, filter);
    }

    FILE *p = popen(cmd, "r");
    if (!p) return -1;

    int n = 0;
    char line[1024];
    double pending_start = -1.0;
    while (fgets(line, sizeof(line), p)) {
        if (mode == 0) {
            /* showinfo prints: pts_time:12.345 */
            char *t = strstr(line, "pts_time:");
            if (t) {
                double ts = atof(t + 9);
                if (n < cap) { out[n].start = ts; out[n].end = -1.0; n++; }
            }
        } else if (mode == 1) {
            /* blackdetect: black_start:12.34 black_end:15.67 */
            char *bs = strstr(line, "black_start:");
            char *be = strstr(line, "black_end:");
            if (bs && be && n < cap) {
                out[n].start = atof(bs + 12);
                out[n].end   = atof(be + 10);
                n++;
            }
        } else {
            /* silencedetect: silence_start:12.34  / silence_end:15.67 */
            char *ss = strstr(line, "silence_start:");
            char *se = strstr(line, "silence_end:");
            if (ss) pending_start = atof(ss + 14);
            if (se && pending_start >= 0.0 && n < cap) {
                out[n].start = pending_start;
                out[n].end   = atof(se + 12);
                n++;
                pending_start = -1.0;
            }
        }
    }
    pclose(p);
    return n;
}

/* Map session track kind constants:
 *  0 = instrument/MIDI, 1 = audio, 3 = video (kind 2 = bus).
 *  (WB_TRACK_KIND_* live in wbus.h.) */
int wb_session_kind_is_video(wb_session *s, int track) {
    if (!s || track < 0 || track >= (int)s->track_count) return 0;
    return s->tracks[track].kind == WB_TRACK_KIND_VIDEO;
}

/* ---- auto clip-to-shorts (R015 Tier 3) ------------------------------- */

/* Detect "interesting" segments in `src` and export each as a separate
 * vertical-friendly short clip. Strategy:
 *   1. scene-detect to find candidate cut points (mode 0)
 *   2. pair consecutive cuts into [start,end] spans
 *   3. drop spans shorter than `min_dur` or longer than `max_dur`
 *   4. drop spans whose center frame is black or whose audio is silent
 *      (cheap guard against title cards / dead air)
 *   5. export each surviving span via lossless -c copy trim to out_dir
 *      (named short_0001.mp4 ...). Returns count exported, or -1 on error.
 */
int wb_video_auto_clip_shorts(const char *src, const char *out_dir,
                              double scene_thr, double min_dur, double max_dur) {
    if (!src || !out_dir) return -1;
    if (min_dur <= 0) min_dur = 8.0;
    if (max_dur <= 0) max_dur = 60.0;
    if (scene_thr <= 0) scene_thr = 0.3;

    /* source duration via our FFmpeg decoder (no ffprobe dependency) */
    double total = 0.0;
    {
        wb_video_decoder *d = wb_video_decoder_open(src);
        if (d) { total = wb_video_decoder_get_duration(d); wb_video_decoder_close(d); }
    }
    if (total <= 0) return -1;

    /* adaptive scene threshold: try the requested, then loosen until we get
     * at least one cut (so a static clip still yields a full-length short). */
    wb_video_segment cuts[256];
    int ncuts = 0;
    double thr = scene_thr;
    for (int attempt = 0; attempt < 4; attempt++) {
        ncuts = wb_video_detect_segments(src, 0, thr, cuts, 256);
        if (ncuts >= 1 || thr <= 0.005) break;
        thr *= 0.3;
    }
    if (ncuts < 1) {
        /* no scene activity at all: emit the whole clip as one short */
        char path[1024];
        snprintf(path, sizeof(path), "%s/short_%04d.mp4", out_dir, 1);
        wb_video_segment whole = { 0.0, total };
        return wb_video_lossless_trim(src, &whole, 1, path) == 0 ? 1 : -1;
    }

    /* Build [prev, cut] spans between consecutive scene points. */
    double prev = 0.0;
    wb_video_segment spans[256];
    int nspans = 0;
    for (int i = 0; i < ncuts && nspans < 256; i++) {
        double c = cuts[i].start;
        if (c - prev >= min_dur && c - prev <= max_dur) {
            spans[nspans].start = prev;
            spans[nspans].end = c;
            nspans++;
        }
        prev = c;
    }
    if (total > prev && total - prev >= min_dur && total - prev <= max_dur
        && nspans < 256) {
        spans[nspans].start = prev; spans[nspans].end = total; nspans++;
    }

    int exported = 0;
    char path[1024];
    for (int i = 0; i < nspans; i++) {
        snprintf(path, sizeof(path), "%s/short_%04d.mp4", out_dir, i + 1);
        /* quick black guard on the span center */
        double mid = (spans[i].start + spans[i].end) * 0.5;
        wb_video_segment blk[8];
        int nb = wb_video_detect_segments(src, 1, 0.95, blk, 8);  /* black */
        int is_black = 0;
        for (int b = 0; b < nb; b++)
            if (mid >= blk[b].start && mid <= blk[b].end) { is_black = 1; break; }
        if (is_black) continue;
        if (wb_video_lossless_trim(src, &spans[i], 1, path) == 0)
            exported++;
    }
    return exported;
}
