/* wb_audio_mux.c — audio muxing for video export (R085).
 *
 * Post-processes an MP4 file to add an AAC audio track from the edit
 * graph's audio clips. This is cleaner than modifying the video render
 * function — we render video first, then mux audio in a second pass.
 *
 * Usage:
 *   1. wb_edit_render_to_mp4() renders video-only MP4
 *   2. wb_audio_mux_to_mp4() adds audio track to the MP4
 *
 * This two-pass approach is how many NLEs work internally.
 */

#include "wbus/wbus_edit.h"
#include "wbus/wbus_video.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2

/* Add an AAC audio track to an existing MP4 file.
 * g: edit graph with audio clips
 * mp4_path: path to the video-only MP4 file
 * cancel: optional cancel flag
 * Returns 0 on success, -1 on error.
 */
int wb_audio_mux_to_mp4(wb_edit_graph *g, const char *mp4_path,
                          volatile int *cancel) {
    if (!g || !mp4_path) return -1;

    /* Check if there are any audio clips */
    int has_audio = 0;
    for (uint32_t t = 0; t < g->track_count; t++) {
        if (g->tracks[t].audio_clip_count > 0) { has_audio = 1; break; }
    }
    if (!has_audio) return 0;  /* Nothing to do */

    /* Get audio duration */
    double audio_dur = wb_audio_get_duration(g);
    if (audio_dur <= 0) return 0;

    /* Temporary WAV file for the mixed audio */
    char wav_path[512];
    snprintf(wav_path, sizeof(wav_path), "%s_audio.wav", mp4_path);

    /* Mix audio to a temporary WAV file */
    int total_samples = (int)(audio_dur * AUDIO_SAMPLE_RATE);
    float *mix_buf = calloc(total_samples * AUDIO_CHANNELS, sizeof(float));
    if (!mix_buf) return -1;

    wb_audio_mix(g, mix_buf, 0.0, total_samples);

    /* Write as 16-bit PCM WAV */
    /* Convert float to int16 */
    int16_t *pcm16 = malloc(total_samples * AUDIO_CHANNELS * sizeof(int16_t));
    if (!pcm16) { free(mix_buf); return -1; }
    for (int i = 0; i < total_samples * AUDIO_CHANNELS; i++) {
        float s = mix_buf[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm16[i] = (int16_t)(s * 32767.0f);
    }
    free(mix_buf);

    /* Write WAV file */
    FILE *wav = fopen(wav_path, "wb");
    if (!wav) { free(pcm16); return -1; }

    /* WAV header */
    uint32_t data_size = total_samples * AUDIO_CHANNELS * 2;
    uint32_t file_size = 36 + data_size;
    uint32_t byte_rate = AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * 2;
    uint16_t block_align = AUDIO_CHANNELS * 2;
    uint16_t bits_per_sample = 16;

    fwrite("RIFF", 1, 4, wav);
    uint32_t v = file_size; fwrite(&v, 4, 1, wav);
    fwrite("WAVE", 1, 4, wav);
    fwrite("fmt ", 1, 4, wav);
    v = 16; fwrite(&v, 4, 1, wav);
    uint16_t fmt = 1; fwrite(&fmt, 2, 1, wav);
    uint16_t channels = AUDIO_CHANNELS;
    fwrite(&channels, 2, 1, wav);
    v = AUDIO_SAMPLE_RATE; fwrite(&v, 4, 1, wav);
    v = byte_rate; fwrite(&v, 4, 1, wav);
    fwrite(&block_align, 2, 1, wav);
    fwrite(&bits_per_sample, 2, 1, wav);
    fwrite("data", 1, 4, wav);
    v = data_size; fwrite(&v, 4, 1, wav);
    fwrite(pcm16, 1, data_size, wav);
    fclose(wav);
    free(pcm16);

    /* Now mux the WAV into the MP4 using libav */
    /* Open the video MP4 */
    AVFormatContext *video_fmt = NULL;
    if (avformat_open_input(&video_fmt, mp4_path, NULL, NULL) < 0) {
        fprintf(stderr, "audio_mux: cannot open %s\n", mp4_path);
        remove(wav_path);
        return -1;
    }
    avformat_find_stream_info(video_fmt, NULL);

    /* Create output MP4 */
    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s_final.mp4", mp4_path);

    AVFormatContext *out_fmt = NULL;
    avformat_alloc_output_context2(&out_fmt, NULL, NULL, out_path);
    if (!out_fmt) {
        avformat_close_input(&video_fmt);
        remove(wav_path);
        return -1;
    }

    /* Copy video stream */
    AVStream *video_out = avformat_new_stream(out_fmt, NULL);
    AVStream *video_in = NULL;
    for (unsigned i = 0; i < video_fmt->nb_streams; i++) {
        if (video_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_in = video_fmt->streams[i];
            break;
        }
    }
    if (!video_in || avcodec_parameters_copy(video_out->codecpar, video_in->codecpar) < 0) {
        avformat_close_input(&video_fmt);
        remove(wav_path);
        return -1;
    }
    video_out->time_base = video_in->time_base;

    /* Add AAC audio stream */
    const AVCodec *aac_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVStream *audio_out = NULL;
    AVCodecContext *audio_enc = NULL;
    if (aac_codec) {
        audio_out = avformat_new_stream(out_fmt, aac_codec);
        if (audio_out) {
            audio_enc = avcodec_alloc_context3(aac_codec);
            if (audio_enc) {
                audio_enc->sample_rate = AUDIO_SAMPLE_RATE;
                audio_enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
                audio_enc->bit_rate = 128000;
                AVChannelLayout ch_layout = AV_CHANNEL_LAYOUT_STEREO;
                av_channel_layout_copy(&audio_enc->ch_layout, &ch_layout);
                audio_enc->time_base = (AVRational){1, AUDIO_SAMPLE_RATE};
                if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER)
                    audio_enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                avcodec_open2(audio_enc, aac_codec, NULL);
                avcodec_parameters_from_context(audio_out->codecpar, audio_enc);
            }
        }
    }

    /* Open output */
    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        avio_open(&out_fmt->pb, out_path, AVIO_FLAG_WRITE);
    }
    avformat_write_header(out_fmt, NULL);

    /* Copy video packets */
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(video_fmt, pkt) >= 0) {
        if (cancel && *cancel) break;
        if (pkt->stream_index == video_in->index) {
            av_packet_rescale_ts(pkt, video_in->time_base, video_out->time_base);
            pkt->stream_index = video_out->index;
            av_interleaved_write_frame(out_fmt, pkt);
        }
        av_packet_unref(pkt);
    }

    /* Encode and write audio if we have an audio encoder */
    if (audio_enc && audio_out) {
        /* Read WAV and encode */
        FILE *wav_in = fopen(wav_path, "rb");
        if (wav_in) {
            /* Skip WAV header (44 bytes) */
            fseek(wav_in, 44, SEEK_SET);

            AVFrame *frame = av_frame_alloc();
            frame->nb_samples = audio_enc->frame_size;
            frame->format = audio_enc->sample_fmt;
            frame->sample_rate = audio_enc->sample_rate;
            av_channel_layout_copy(&frame->ch_layout, &audio_enc->ch_layout);
            av_frame_get_buffer(frame, 0);

            SwrContext *swr = swr_alloc();
            AVChannelLayout in_l = AV_CHANNEL_LAYOUT_STEREO;
            av_opt_set_chlayout(swr, "in_chlayout", &in_l, 0);
            av_opt_set_int(swr, "in_sample_rate", AUDIO_SAMPLE_RATE, 0);
            av_opt_set_sample_fmt(swr, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
            av_opt_set_chlayout(swr, "out_chlayout", &audio_enc->ch_layout, 0);
            av_opt_set_int(swr, "out_sample_rate", AUDIO_SAMPLE_RATE, 0);
            av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
            swr_init(swr);

            int64_t audio_pts = 0;
            int16_t *read_buf = malloc(audio_enc->frame_size * AUDIO_CHANNELS * 2);

            while (!feof(wav_in)) {
                if (cancel && *cancel) break;
                int n = fread(read_buf, 2, audio_enc->frame_size * AUDIO_CHANNELS, wav_in);
                if (n <= 0) break;

                /* Convert S16 interleaved -> FLTP planar */
                const uint8_t *in_data[1] = { (uint8_t *)read_buf };
                uint8_t *out_data[2] = { frame->data[0], frame->data[1] };
                swr_convert(swr, out_data, audio_enc->frame_size, in_data, n / AUDIO_CHANNELS);

                frame->pts = audio_pts;
                audio_pts += audio_enc->frame_size;

                if (avcodec_send_frame(audio_enc, frame) == 0) {
                    AVPacket *apkt = av_packet_alloc();
                    while (avcodec_receive_packet(audio_enc, apkt) == 0) {
                        av_packet_rescale_ts(apkt, audio_enc->time_base, audio_out->time_base);
                        apkt->stream_index = audio_out->index;
                        av_interleaved_write_frame(out_fmt, apkt);
                        av_packet_unref(apkt);
                    }
                    av_packet_free(&apkt);
                }
            }

            /* Flush encoder */
            avcodec_send_frame(audio_enc, NULL);
            AVPacket *apkt = av_packet_alloc();
            while (avcodec_receive_packet(audio_enc, apkt) == 0) {
                av_packet_rescale_ts(apkt, audio_enc->time_base, audio_out->time_base);
                apkt->stream_index = audio_out->index;
                av_interleaved_write_frame(out_fmt, apkt);
                av_packet_unref(apkt);
            }
            av_packet_free(&apkt);

            free(read_buf);
            swr_free(&swr);
            av_frame_free(&frame);
            fclose(wav_in);
        }
    }

    av_write_trailer(out_fmt);

    /* Cleanup */
    av_packet_free(&pkt);
    if (audio_enc) avcodec_free_context(&audio_enc);
    if (out_fmt && !(out_fmt->oformat->flags & AVFMT_NOFILE))
        avio_closep(&out_fmt->pb);
    avformat_free_context(out_fmt);
    avformat_close_input(&video_fmt);

    /* Replace original MP4 with the one that has audio */
    remove(mp4_path);
    rename(out_path, mp4_path);
    remove(wav_path);

    return 0;
}
