/* wb_compositor_encode.c — compositor → H.264 encoder via libav (no ffmpeg CLI).
 *
 * R084 gap-analysis §2 (CRITICAL): the existing wb_compositor_export_mp4()
 * shells out to ffmpeg CLI. This module provides wb_compositor_render_to_mp4(),
 * a render loop that pulls frames from the node graph at fps intervals and
 * encodes them directly via libavformat/libavcodec/libswscale.
 *
 * Contract:
 *   - Pull a wb_frame (RGBA float, 0..1) from wb_node_pull(root, t, 0,0,w,h)
 *   - Convert RGBA float → YUV420P uint8 via SwsContext
 *   - Encode via avcodec_send_frame / avcodec_receive_packet
 *   - Mux into MP4 via av_write_frame
 *   - Honor *cancel between frames; call prog(ctx, 0..1) for progress
 *
 * Returns 0 on success, -1 on error, -2 if cancelled.
 */

#include "wbus/wbus_compositor.h"
#include "wbus/wbus.h"   /* wb_export_prog_fn */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* ---- internals ---------------------------------------------------------- */

/* The encoder pipeline. We allocate an AVFormatContext (mp4 muxer), add one
 * video stream with a libx264 AVCodecContext, and a SwsContext for the
 * RGBA-float → YUV420P conversion. The source frame buffer is pre-allocated
 * in RGBA uint8 (the SwsContext input); we convert float RGBA → uint8 RGBA
 * in a tight loop before the colorspace convert. */
typedef struct {
    AVFormatContext *fmt_ctx;
    AVCodecContext  *codec_ctx;
    AVStream        *stream;
    SwsContext      *sws_ctx;
    AVFrame         *yuv_frame;
    AVPacket        *pkt;
    int64_t          pts;        /* monotonically increasing */
    int              w, h;
    /* scratch: RGBA uint8 source buffer (sws input) */
    uint8_t         *rgba_buf;
} enc_pipe;

/* Convert wb_frame (RGBA float 0..1) into the encoder's RGBA uint8 scratch.
 * Clamps to [0,255]. Alpha is ignored (treated as opaque). */
static void frame_float_to_rgba8(const wb_frame *f, uint8_t *dst) {
    int n = f->w * f->h;
    for (int i = 0; i < n; i++) {
        float r = f->px[i].r, g = f->px[i].g, b = f->px[i].b;
        /* clamp */
        if (r < 0.f) r = 0.f; else if (r > 1.f) r = 1.f;
        if (g < 0.f) g = 0.f; else if (g > 1.f) g = 1.f;
        if (b < 0.f) b = 0.f; else if (b > 1.f) b = 1.f;
        dst[i * 4 + 0] = (uint8_t)(r * 255.f + 0.5f);
        dst[i * 4 + 1] = (uint8_t)(g * 255.f + 0.5f);
        dst[i * 4 + 2] = (uint8_t)(b * 255.f + 0.5f);
        dst[i * 4 + 3] = 255;
    }
}

/* Initialize the encoder pipeline. Returns 0 on success, -1 on error. */
static int enc_open(enc_pipe *enc, const char *out_path, int w, int h,
                    double fps) {
    memset(enc, 0, sizeof(*enc));
    enc->w = w;
    enc->h = h;

    /* Allocate output format context for mp4. */
    int rc = avformat_alloc_output_context2(&enc->fmt_ctx, NULL, "mp4", out_path);
    if (rc < 0 || !enc->fmt_ctx) {
        fprintf(stderr, "wb_compositor_encode: avformat_alloc_output_context2 failed\n");
        return -1;
    }

    /* Find libx264 encoder. */
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "wb_compositor_encode: libx264 encoder not found\n");
        return -1;
    }

    enc->stream = avformat_new_stream(enc->fmt_ctx, NULL);
    if (!enc->stream) {
        fprintf(stderr, "wb_compositor_encode: avformat_new_stream failed\n");
        return -1;
    }

    enc->codec_ctx = avcodec_alloc_context3(codec);
    if (!enc->codec_ctx) {
        fprintf(stderr, "wb_compositor_encode: avcodec_alloc_context3 failed\n");
        return -1;
    }

    /* Encoder parameters. */
    enc->codec_ctx->width     = w;
    enc->codec_ctx->height    = h;
    enc->codec_ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    enc->codec_ctx->time_base = (AVRational){1, (int)(fps + 0.5)};
    enc->codec_ctx->framerate = (AVRational){(int)(fps + 0.5), 1};
    enc->codec_ctx->bit_rate  = 0;   /* CRF mode; 0 = use crf */
    enc->codec_ctx->gop_size  = (int)(fps + 0.5);  /* 1-second GOP */
    enc->codec_ctx->max_b_frames = 1;

    /* CRF 23, preset veryfast via private options. */
    av_opt_set(enc->codec_ctx->priv_data, "preset", "veryfast", 0);
    av_opt_set(enc->codec_ctx->priv_data, "crf", "23", 0);

    /* Some mp4 muxers require a profile flag. */
    av_opt_set(enc->codec_ctx->priv_data, "profile", "baseline", 0);

    /* Open the codec. */
    rc = avcodec_open2(enc->codec_ctx, codec, NULL);
    if (rc < 0) {
        fprintf(stderr, "wb_compositor_encode: avcodec_open2 failed: %s\n",
                av_err2str(rc));
        return -1;
    }

    /* Copy codec parameters into the stream. */
    rc = avcodec_parameters_from_context(enc->stream->codecpar, enc->codec_ctx);
    if (rc < 0) {
        fprintf(stderr, "wb_compositor_encode: avcodec_parameters_from_context failed\n");
        return -1;
    }

    enc->stream->time_base = enc->codec_ctx->time_base;

    /* Allocate the YUV420P frame that we feed into the encoder. */
    enc->yuv_frame = av_frame_alloc();
    if (!enc->yuv_frame) return -1;
    enc->yuv_frame->format = AV_PIX_FMT_YUV420P;
    enc->yuv_frame->width  = w;
    enc->yuv_frame->height = h;
    rc = av_frame_get_buffer(enc->yuv_frame, 32);  /* 32-byte aligned */
    if (rc < 0) {
        fprintf(stderr, "wb_compositor_encode: av_frame_get_buffer failed\n");
        return -1;
    }

    /* Allocate the RGBA uint8 scratch buffer. */
    enc->rgba_buf = (uint8_t *)malloc(w * h * 4);
    if (!enc->rgba_buf) return -1;

    /* Create the SwsContext: RGBA uint8 → YUV420P. */
    enc->sws_ctx = sws_getContext(w, h, AV_PIX_FMT_RGBA,
                                  w, h, AV_PIX_FMT_YUV420P,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (!enc->sws_ctx) {
        fprintf(stderr, "wb_compositor_encode: sws_getContext failed\n");
        return -1;
    }

    /* Allocate the output packet. */
    enc->pkt = av_packet_alloc();
    if (!enc->pkt) return -1;

    /* Open the output file and write the header. */
    if (!(enc->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&enc->fmt_ctx->pb, out_path, AVIO_FLAG_WRITE);
        if (rc < 0) {
            fprintf(stderr, "wb_compositor_encode: avio_open failed: %s\n",
                    av_err2str(rc));
            return -1;
        }
    }

    rc = avformat_write_header(enc->fmt_ctx, NULL);
    if (rc < 0) {
        fprintf(stderr, "wb_compositor_encode: avformat_write_header failed: %s\n",
                av_err2str(rc));
        return -1;
    }

    enc->pts = 0;
    return 0;
}

/* Encode one YUV420P frame. The frame's pts must be set before calling.
 * Writes any output packets to the muxer. Returns 0 on success, -1 on error. */
static int enc_write_frame(enc_pipe *enc, AVFrame *frame) {
    /* Send frame to encoder. */
    int rc = avcodec_send_frame(enc->codec_ctx, frame);
    if (rc < 0) {
        fprintf(stderr, "wb_compositor_encode: avcodec_send_frame failed: %s\n",
                av_err2str(rc));
        return -1;
    }

    /* Drain available packets. */
    while (rc >= 0) {
        rc = avcodec_receive_packet(enc->codec_ctx, enc->pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
            break;
        if (rc < 0) {
            fprintf(stderr, "wb_compositor_encode: avcodec_receive_packet failed\n");
            return -1;
        }

        /* Rescale timestamp from codec time_base to stream time_base. */
        av_packet_rescale_ts(enc->pkt, enc->codec_ctx->time_base,
                             enc->stream->time_base);
        enc->pkt->stream_index = enc->stream->index;

        rc = av_write_frame(enc->fmt_ctx, enc->pkt);
        av_packet_unref(enc->pkt);
        if (rc < 0) {
            fprintf(stderr, "wb_compositor_encode: av_write_frame failed\n");
            return -1;
        }
    }
    return 0;
}

/* Flush remaining frames from the encoder, then write the trailer. */
static int enc_close(enc_pipe *enc, int flush) {
    int rc = 0;
    if (flush) {
        /* Send NULL frame to drain the encoder. */
        rc = enc_write_frame(enc, NULL);
        if (rc == 0)
            av_write_trailer(enc->fmt_ctx);
    }

    /* Cleanup. */
    if (enc->pkt)        av_packet_free(&enc->pkt);
    if (enc->yuv_frame)  av_frame_free(&enc->yuv_frame);
    if (enc->rgba_buf)   free(enc->rgba_buf);
    if (enc->sws_ctx)    sws_freeContext(enc->sws_ctx);
    if (enc->codec_ctx)  avcodec_free_context(&enc->codec_ctx);
    if (enc->fmt_ctx) {
        if (!(enc->fmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&enc->fmt_ctx->pb);
        avformat_free_context(enc->fmt_ctx);
    }
    memset(enc, 0, sizeof(*enc));
    return rc;
}

/* ---- public API --------------------------------------------------------- */

/* Render the node graph to an H.264 MP4 file via libav.
 *
 * Pulls frames from `root` at each time t = 0, 1/fps, 2/fps, ... up to
 * `duration` seconds. Each frame is converted from RGBA float to YUV420P
 * and encoded. The cancel flag is checked between frames; if set, the
 * function aborts and returns -2. The prog callback (if non-NULL) receives
 * progress in [0,1].
 *
 * Returns:
 *    0  success
 *   -1  error (bad params, encoder failure, no video source)
 *   -2  cancelled via *cancel
 */
int wb_compositor_render_to_mp4(wb_node *root, const char *out_path,
                                double fps, int w, int h, double duration,
                                volatile int *cancel,
                                wb_export_prog_fn prog, void *prog_ctx) {
    if (!root || !out_path || fps <= 0 || w <= 0 || h <= 0 || duration <= 0)
        return -1;

    /* Probe: pull one frame at t=0 to verify the graph produces output.
     * If the first pull returns NULL, the graph has no video source. */
    wb_frame *probe = wb_node_pull(root, 0.0, 0, 0, w, h);
    if (!probe) {
        fprintf(stderr, "wb_compositor_render_to_mp4: graph produced no frame "
                        "(no video source?)\n");
        return -1;
    }
    wb_frame_free(probe);

    int nframes = (int)(duration * fps);
    if (nframes <= 0) return -1;

    enc_pipe enc;
    if (enc_open(&enc, out_path, w, h, fps) != 0)
        return -1;

    /* Source stride for RGBA (4 bytes per pixel). */
    const int src_stride = w * 4;
    const uint8_t *src_slices[1] = { enc.rgba_buf };
    int src_linesize[1] = { src_stride };

    int result = 0;
    for (int i = 0; i < nframes; i++) {
        if (cancel && *cancel) {
            result = -2;
            break;
        }

        double t = (double)i / fps;

        /* Pull frame from the node graph. */
        wb_frame *f = wb_node_pull(root, t, 0, 0, w, h);
        if (!f) {
            fprintf(stderr, "wb_compositor_render_to_mp4: pull failed at t=%.4f\n", t);
            result = -1;
            break;
        }

        /* Convert RGBA float → RGBA uint8 in the scratch buffer. */
        frame_float_to_rgba8(f, enc.rgba_buf);
        wb_frame_free(f);

        /* Colorspace convert RGBA uint8 → YUV420P into enc.yuv_frame. */
        sws_scale(enc.sws_ctx, src_slices, src_linesize,
                  0, h, enc.yuv_frame->data, enc.yuv_frame->linesize);

        /* Set presentation timestamp. */
        enc.yuv_frame->pts = enc.pts++;

        /* Encode + mux. */
        if (enc_write_frame(&enc, enc.yuv_frame) != 0) {
            result = -1;
            break;
        }

        /* Report progress. */
        if (prog)
            prog(prog_ctx, (double)(i + 1) / (double)nframes);
    }

    /* Flush encoder: enc_close sends a NULL frame to drain the encoder,
     * then writes the trailer. On cancel/error we teardown without flushing
     * so we don't write a broken file. */
    int flush = (result == 0);
    int close_rc = enc_close(&enc, flush);
    if (result == 0)
        result = close_rc;
    return result;
}

/* ---- ffmpeg-CLI pipe export (popen/pclose) ---------------------------- */

/* Export the compositor node graph to H.264 MP4 via an ffmpeg pipe.
 * Raw RGBA frames are pulled from the graph and piped to ffmpeg's stdin.
 * Returns 0 on success, -1 on error. */
int wb_compositor_export_graph(wb_node *root, double fps, double duration_sec,
                               const char *output_path, int w, int h) {
    if (!root || !output_path || fps <= 0 || duration_sec <= 0 || w <= 0 || h <= 0)
        return -1;

    /* Build the ffmpeg command line. */
    char cmd[2048];
    int n = snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f rawvideo -pix_fmt rgba -s %dx%d -r %.4f -i pipe:0 "
        "-c:v libx264 -pix_fmt yuv420p -preset fast -crf 18 \"%s\"",
        w, h, fps, output_path);
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        fprintf(stderr, "wb_compositor_export_graph: command buffer overflow\n");
        return -1;
    }

    /* Open the pipe. */
    FILE *pipe = popen(cmd, "w");
    if (!pipe) {
        perror("wb_compositor_export_graph: popen failed");
        return -1;
    }

    int nframes = (int)(duration_sec * fps);
    if (nframes <= 0) nframes = 1;

    int result = 0;
    for (int i = 0; i < nframes; i++) {
        double t = (double)i / fps;

        /* Pull frame from the node graph. */
        wb_frame *f = wb_node_pull(root, t, 0, 0, w, h);
        if (!f) {
            fprintf(stderr, "wb_compositor_export_graph: pull failed at t=%.4f\n", t);
            result = -1;
            break;
        }

        /* Write raw RGBA bytes (4 bytes per pixel, row-major). */
        size_t frame_bytes = (size_t)w * h * 4;
        size_t written = fwrite(f->px, 1, frame_bytes, pipe);
        wb_frame_free(f);

        if (written != frame_bytes) {
            fprintf(stderr, "wb_compositor_export_graph: short write at t=%.4f "
                            "(%zu of %zu)\n", t, written, frame_bytes);
            result = -1;
            break;
        }
    }

    /* Close the pipe and check ffmpeg's exit status. */
    int rc = pclose(pipe);
    if (rc != 0) {
        fprintf(stderr, "wb_compositor_export_graph: ffmpeg exited with %d\n", rc);
        result = -1;
    }

    return result;
}

/* Export the compositor node graph to H.264 MP4 with an audio track.
 * First encodes video to a temp file, then muxes with audio via ffmpeg.
 * Returns 0 on success, -1 on error. */
int wb_compositor_export_graph_with_audio(wb_node *root, double fps,
                                          double duration_sec,
                                          const char *audio_wav_path,
                                          const char *output_path,
                                          int w, int h) {
    if (!root || !audio_wav_path || !output_path || fps <= 0 || duration_sec <= 0)
        return -1;

    /* Build a temp video file path in the same directory as the output. */
    char temp_video[1024];
    /* Use a simple temp name based on output path + .tmp.mp4 */
    int n = snprintf(temp_video, sizeof(temp_video), "%s.tmp.mp4", output_path);
    if (n < 0 || (size_t)n >= sizeof(temp_video)) {
        fprintf(stderr, "wb_compositor_export_graph_with_audio: path overflow\n");
        return -1;
    }

    /* Step 1: encode video to temp file. */
    int rc = wb_compositor_export_graph(root, fps, duration_sec, temp_video, w, h);
    if (rc != 0) {
        fprintf(stderr, "wb_compositor_export_graph_with_audio: video encode failed\n");
        return -1;
    }

    /* Step 2: mux video + audio into the final output. */
    char mux_cmd[2048];
    n = snprintf(mux_cmd, sizeof(mux_cmd),
        "ffmpeg -y -i \"%s\" -i \"%s\" -c:v copy -c:a aac -shortest \"%s\"",
        temp_video, audio_wav_path, output_path);
    if (n < 0 || (size_t)n >= sizeof(mux_cmd)) {
        fprintf(stderr, "wb_compositor_export_graph_with_audio: mux command overflow\n");
        remove(temp_video);
        return -1;
    }

    FILE *pipe = popen(mux_cmd, "w");
    if (!pipe) {
        perror("wb_compositor_export_graph_with_audio: popen mux failed");
        remove(temp_video);
        return -1;
    }

    int mux_rc = pclose(pipe);

    /* Step 3: remove the temp file regardless of mux result. */
    remove(temp_video);

    if (mux_rc != 0) {
        fprintf(stderr, "wb_compositor_export_graph_with_audio: "
                "ffmpeg mux exited with %d\n", mux_rc);
        return -1;
    }

    return 0;
}