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

/* Simple first-order low-pass filter state for LFE channel.
 * Cutoff ~120 Hz per THX/ITU-R 775 spec for LFE. */
static float lfe_lowpass(float input, float *state, float cutoff_hz, float sample_rate) {
    float rc = 1.0f / (2.0f * 3.14159265f * cutoff_hz);
    float dt = 1.0f / sample_rate;
    float alpha = dt / (rc + dt);
    *state += alpha * (input - *state);
    return *state;
}

/* Mix audio from all tracks into 5.1 surround (6 channels: L, R, C, LFE, Ls, Rs).
 * Uses per-clip pan position to distribute signal:
 *   - pan == 0.0 (center): signal goes to C (center) at full gain
 *   - pan < 0.0 (left):    signal goes to L + Ls, gain scales from center to full left
 *   - pan > 0.0 (right):   signal goes to R + Rs, gain scales from center to full right
 *
 * Gain law (constant-power style):
 *   For pan in [-1, 0] (left side):
 *     L  gain = 1.0 (full)
 *     Ls gain = |pan| (0 at center, 1 at full left)
 *     C  gain = 1.0 - |pan| (1 at center, 0 at full left)
 *   For pan in [0, 1] (right side):
 *     R  gain = 1.0 (full)
 *     Rs gain = pan (0 at center, 1 at full right)
 *     C  gain = 1.0 - pan (1 at center, 0 at full right)
 *
 * LFE gets a 120 Hz low-pass of the summed mono mix.
 */
int wb_audio_mix_surround(wb_edit_graph *g, float *ch_L, float *ch_R,
                          float *ch_C, float *ch_LFE, float *ch_Ls, float *ch_Rs,
                          double start_time, int n_frames) {
    if (!g || !ch_L || !ch_R || !ch_C || !ch_LFE || !ch_Ls || !ch_Rs || n_frames <= 0)
        return 0;

    int contributed = 0;

    /* Clear all 6 channel outputs */
    memset(ch_L, 0, n_frames * sizeof(float));
    memset(ch_R, 0, n_frames * sizeof(float));
    memset(ch_C, 0, n_frames * sizeof(float));
    memset(ch_LFE, 0, n_frames * sizeof(float));
    memset(ch_Ls, 0, n_frames * sizeof(float));
    memset(ch_Rs, 0, n_frames * sizeof(float));

    /* Temporary mono mix for LFE (sum of all channels before low-pass) */
    float *mono_mix = calloc(n_frames, sizeof(float));
    if (!mono_mix) return 0;

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

            /* Compute per-clip pan gains */
            float pan = clip->pan;  /* -1 (left) .. +1 (right), 0 = center */
            if (pan < -1.0f) pan = -1.0f;
            if (pan > 1.0f) pan = 1.0f;

            float gain_L, gain_R, gain_C, gain_Ls, gain_Rs;

            if (pan == 0.0f) {
                /* Center: only C channel */
                gain_C  = 1.0f;
                gain_L  = 0.0f;
                gain_R  = 0.0f;
                gain_Ls = 0.0f;
                gain_Rs = 0.0f;
            } else if (pan < 0.0f) {
                /* Left side: pan in [-1, 0) */
                float abs_pan = -pan;  /* 0..1 */
                gain_L  = 1.0f;
                gain_Ls = abs_pan;
                gain_C  = 1.0f - abs_pan;
                gain_R  = 0.0f;
                gain_Rs = 0.0f;
            } else {
                /* Right side: pan in (0, 1] */
                gain_R  = 1.0f;
                gain_Rs = pan;
                gain_C  = 1.0f - pan;
                gain_L  = 0.0f;
                gain_Ls = 0.0f;
            }

            /* Mix into each channel */
            for (int f = start_frame; f < end_frame; f++) {
                /* Calculate source frame */
                double timeline_offset = (double)f / AUDIO_SAMPLE_RATE + start_time - clip_start_time;
                double source_offset = clip->start_in_source + timeline_offset * clip->speed;
                int src_frame = (int)(source_offset * audio_sr);

                if (src_frame < 0 || (uint32_t)src_frame >= audio_frames) continue;

                /* Get source sample (mono or stereo downmix) */
                float sample;
                if (audio_ch >= 2) {
                    sample = (audio_data[src_frame * 2] + audio_data[src_frame * 2 + 1]) * 0.5f;
                } else {
                    sample = audio_data[src_frame];
                }

                /* Apply volume */
                float s = sample * clip->volume;

                /* Distribute to channels based on pan gains */
                ch_L[f]  += s * gain_L;
                ch_R[f]  += s * gain_R;
                ch_C[f]  += s * gain_C;
                ch_Ls[f] += s * gain_Ls;
                ch_Rs[f] += s * gain_Rs;

                /* Accumulate mono mix for LFE */
                mono_mix[f] += s;
            }

            free(audio_data);
            contributed++;
        }
    }

    /* Apply 120 Hz low-pass to mono mix for LFE */
    float lfe_state = 0.0f;
    for (int f = 0; f < n_frames; f++) {
        ch_LFE[f] = lfe_lowpass(mono_mix[f], &lfe_state, 120.0f, (float)AUDIO_SAMPLE_RATE);
    }

    free(mono_mix);

    /* Soft clip all channels to prevent clipping */
    if (contributed > 1) {
        for (int f = 0; f < n_frames; f++) {
            float *ch;
            for (int c = 0; c < 6; c++) {
                switch (c) {
                    case 0: ch = ch_L; break;
                    case 1: ch = ch_R; break;
                    case 2: ch = ch_C; break;
                    case 3: ch = ch_LFE; break;
                    case 4: ch = ch_Ls; break;
                    default: ch = ch_Rs; break;
                }
                if (ch[f] > 1.0f) ch[f] = 1.0f;
                if (ch[f] < -1.0f) ch[f] = -1.0f;
            }
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
