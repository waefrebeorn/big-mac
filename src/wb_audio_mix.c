/* wb_audio_mix.c — audio mixing for video export (R085).
 *
 * Mixes multiple audio clips into a single PCM stream for MP4 muxing.
 * Each audio clip has: source WAV, start_in_source, duration, timeline_pos,
 * volume, speed.
 *
 * The mixer produces interleaved float samples at the edit graph's sample
 * rate (48000 Hz). These are then encoded as AAC via libav and muxed into
 * the MP4 container alongside the video stream.
 *
 * C11, no third party beyond libav.
 */

#include "wbus/wbus_edit.h"
#include "wb_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2

/* Mix audio from all tracks into an interleaved float buffer.
 * buf: output buffer (interleaved LRLRLR...), must be n_frames * AUDIO_CHANNELS floats
 * g: edit graph
 * start_time: timeline start time in seconds
 * n_frames: number of frames to mix
 * Returns number of clips that contributed.
 */
int wb_audio_mix(wb_edit_graph *g, float *buf, double start_time,
                  int n_frames) {
    if (!g || !buf || n_frames <= 0) return 0;

    int contributed = 0;
    int total_samples = n_frames * AUDIO_CHANNELS;

    /* Clear output */
    memset(buf, 0, total_samples * sizeof(float));

    for (uint32_t trk = 0; trk < g->track_count; trk++) {
        wb_edit_track *tr = &g->tracks[trk];
        if (tr->muted) continue;

        for (uint32_t ac = 0; ac < tr->audio_clip_count; ac++) {
            wb_edit_audio_clip *clip = &tr->audio_clips[ac];

            /* Calculate which output frames this clip covers */
            double clip_start_time = clip->timeline_pos;
            double clip_end_time = clip_start_time + clip->duration;

            int start_frame = (int)((clip_start_time - start_time) * AUDIO_SAMPLE_RATE);
            int end_frame = (int)((clip_end_time - start_time) * AUDIO_SAMPLE_RATE);

            /* Skip if clip is outside our time range */
            if (end_frame < 0 || start_frame >= n_frames) continue;

            /* Clamp to our range */
            if (start_frame < 0) start_frame = 0;
            if (end_frame > n_frames) end_frame = n_frames;

            /* Read audio from source */
            float *audio_data = NULL;
            uint32_t audio_frames = 0;
            int audio_ch = 0;
            int audio_sr = 0;

            if (wb_wav_read_pcm16(clip->source_path, &audio_data, &audio_frames,
                                   &audio_ch, &audio_sr) != 0 || !audio_data) {
                continue;
            }

            /* Mix into output */
            for (int f = start_frame; f < end_frame; f++) {
                /* Calculate source frame (accounting for start_in_source and speed) */
                double timeline_offset = (double)f / AUDIO_SAMPLE_RATE + start_time - clip_start_time;
                double source_offset = clip->start_in_source + timeline_offset * clip->speed;
                int src_frame = (int)(source_offset * audio_sr);

                if (src_frame < 0 || (uint32_t)src_frame >= audio_frames) continue;

                /* Get source sample (mono or stereo) */
                float sample;
                if (audio_ch >= 2) {
                    /* Stereo: mix down to mono for simplicity */
                    sample = (audio_data[src_frame * 2] + audio_data[src_frame * 2 + 1]) * 0.5f;
                } else {
                    sample = audio_data[src_frame];
                }

                /* Apply volume and add to output (interleaved stereo) */
                float s = sample * clip->volume;
                buf[f * AUDIO_CHANNELS] += s;
                buf[f * AUDIO_CHANNELS + 1] += s;
            }

            free(audio_data);
            contributed++;
        }
    }

    /* Normalize to prevent clipping */
    if (contributed > 1) {
        for (int i = 0; i < total_samples; i++) {
            /* Soft clip */
            if (buf[i] > 1.0f) buf[i] = 1.0f;
            if (buf[i] < -1.0f) buf[i] = -1.0f;
        }
    }

    return contributed;
}

/* Get the total audio duration in seconds (max of all clip end times).
 * Returns 0 if no audio clips.
 */
double wb_audio_get_duration(const wb_edit_graph *g) {
    if (!g) return 0.0;
    double max_dur = 0.0;
    for (uint32_t trk = 0; trk < g->track_count; trk++) {
        const wb_edit_track *tr = &g->tracks[trk];
        for (uint32_t ac = 0; ac < tr->audio_clip_count; ac++) {
            const wb_edit_audio_clip *clip = &tr->audio_clips[ac];
            double end = clip->timeline_pos + clip->duration;
            if (end > max_dur) max_dur = end;
        }
    }
    return max_dur;
}
