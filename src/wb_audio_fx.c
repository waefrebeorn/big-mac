/* wb_audio_fx.c — audio effects DSP (R086).
 *
 * Per-clip audio effects for the edit graph:
 *   - 3-band EQ (low/mid/high tone control)
 *   - Reverb (comb filter)
 *   - Compressor (dynamic range)
 *   - Delay/echo
 *   - Distortion (soft clip)
 *   - Chorus (modulated delay)
 *   - VST3 plugin hosting
 *
 * Pure C11, no third party.
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "wbus/wbus_vst3.h"

/* ---- 3-band EQ ---------------------------------------------------------- */
/* Simple shelving EQ: low shelf, mid peak, high shelf */
void wb_audio_fx_eq(float *buf, int n_frames, int channels, int sample_rate,
                    float low_gain, float mid_gain, float high_gain) {
    if (!buf || n_frames <= 0) return;

    /* Filter coefficients (simple 1-pole) */
    float low_coeff = expf(-2.0f * 3.14159f * 200.0f / sample_rate);
    float high_coeff = expf(-2.0f * 3.14159f * 4000.0f / sample_rate);

    float low_state[2] = {0}, high_state[2] = {0};

    for (int i = 0; i < n_frames; i++) {
        for (int ch = 0; ch < channels; ch++) {
            int idx = i * channels + ch;
            float x = buf[idx];

            /* Low shelf */
            low_state[ch] = low_coeff * low_state[ch] + (1.0f - low_coeff) * x;
            float low = low_state[ch];

            /* High shelf */
            high_state[ch] = high_coeff * high_state[ch] + (1.0f - high_coeff) * x;
            float high = x - high_state[ch];

            /* Mid = total - low - high */
            float mid = x - low - high;

            /* Apply gains */
            buf[idx] = low * low_gain + mid * mid_gain + high * high_gain;
        }
    }
}

/* ---- Reverb (simple comb filter) ---------------------------------------- */
void wb_audio_fx_reverb(float *buf, int n_frames, int channels, int sample_rate,
                         float decay, float mix) {
    if (!buf || n_frames <= 0 || decay <= 0) return;

    /* Comb filter delay lines */
    int delay_samples = (int)(0.03f * sample_rate);  /* 30ms */
    if (delay_samples <= 0) delay_samples = 1;

    float *delay_line = calloc(delay_samples * channels, sizeof(float));
    if (!delay_line) return;

    int write_idx = 0;
    for (int i = 0; i < n_frames; i++) {
        for (int ch = 0; ch < channels; ch++) {
            int idx = i * channels + ch;
            int read_idx = (write_idx * channels + ch);

            float delayed = delay_line[read_idx];
            float input = buf[idx];
            float output = input + delayed * decay;

            delay_line[read_idx] = input;
            buf[idx] = input * (1.0f - mix) + output * mix;
        }
        write_idx = (write_idx + 1) % delay_samples;
    }

    free(delay_line);
}

/* ---- Compressor --------------------------------------------------------- */
void wb_audio_fx_compressor(float *buf, int n_frames, int channels,
                             float threshold, float ratio, float attack,
                             float release) {
    if (!buf || n_frames <= 0) return;

    float env = 0.0f;
    float attack_coeff = attack > 0 ? expf(-1.0f / (attack * 48000.0f)) : 0;
    float release_coeff = release > 0 ? expf(-1.0f / (release * 48000.0f)) : 0;

    for (int i = 0; i < n_frames; i++) {
        /* Get peak level across channels */
        float peak = 0.0f;
        for (int ch = 0; ch < channels; ch++) {
            float abs_s = fabsf(buf[i * channels + ch]);
            if (abs_s > peak) peak = abs_s;
        }

        /* Envelope follower */
        if (peak > env) {
            env = attack_coeff * env + (1.0f - attack_coeff) * peak;
        } else {
            env = release_coeff * env + (1.0f - release_coeff) * peak;
        }

        /* Compute gain reduction */
        float gain = 1.0f;
        if (env > threshold && threshold > 0) {
            float db_over = 20.0f * log10f(env / threshold);
            float db_reduced = db_over * (1.0f - 1.0f / ratio);
            gain = powf(10.0f, -db_reduced / 20.0f);
        }

        /* Apply gain */
        for (int ch = 0; ch < channels; ch++) {
            buf[i * channels + ch] *= gain;
        }
    }
}

/* ---- Delay/Echo ---------------------------------------------------------- */
void wb_audio_fx_delay(float *buf, int n_frames, int channels, int sample_rate,
                        float delay_time, float feedback, float mix) {
    if (!buf || n_frames <= 0 || delay_time <= 0) return;

    int delay_samples = (int)(delay_time * sample_rate);
    if (delay_samples <= 0) delay_samples = 1;
    if (delay_samples > sample_rate * 5) delay_samples = sample_rate * 5;  /* max 5s */

    float *delay_line = calloc(delay_samples * channels, sizeof(float));
    if (!delay_line) return;

    int write_idx = 0;
    for (int i = 0; i < n_frames; i++) {
        for (int ch = 0; ch < channels; ch++) {
            int idx = i * channels + ch;
            int read_idx = (write_idx * channels + ch);

            float delayed = delay_line[read_idx];
            float input = buf[idx];

            delay_line[read_idx] = input + delayed * feedback;
            buf[idx] = input * (1.0f - mix) + delayed * mix;
        }
        write_idx = (write_idx + 1) % delay_samples;
    }

    free(delay_line);
}

/* ---- Distortion (soft clip) --------------------------------------------- */
void wb_audio_fx_distortion(float *buf, int n_frames, int channels,
                             float amount) {
    if (!buf || n_frames <= 0 || amount <= 0) return;

    float gain = 1.0f + amount * 10.0f;
    float threshold = 1.0f / gain;

    for (int i = 0; i < n_frames * channels; i++) {
        float x = buf[i] * gain;
        /* Soft clipping using tanh */
        if (x > threshold) {
            buf[i] = threshold + (1.0f - threshold) * tanhf((x - threshold) / (1.0f - threshold));
        } else if (x < -threshold) {
            buf[i] = -(threshold + (1.0f - threshold) * tanhf((-x - threshold) / (1.0f - threshold)));
        } else {
            buf[i] = x;
        }
    }
}

/* ---- Chorus (modulated delay) ------------------------------------------- */
void wb_audio_fx_chorus(float *buf, int n_frames, int channels, int sample_rate,
                         float rate, float depth, float mix) {
    if (!buf || n_frames <= 0) return;

    int max_delay = (int)(0.03f * sample_rate);  /* 30ms max */
    if (max_delay <= 0) max_delay = 1;

    float *delay_line = calloc((max_delay + n_frames) * channels, sizeof(float));
    if (!delay_line) return;

    /* Write input into delay line with extra space */
    memcpy(delay_line, buf, n_frames * channels * sizeof(float));

    float phase = 0.0f;
    for (int i = 0; i < n_frames; i++) {
        /* LFO: sine wave modulation */
        float lfo = sinf(phase) * depth;
        phase += 2.0f * 3.14159f * rate / sample_rate;
        if (phase > 2.0f * 3.14159f) phase -= 2.0f * 3.14159f;

        int delay_samples = (int)((0.01f + lfo) * sample_rate);  /* 10ms base + mod */
        if (delay_samples < 0) delay_samples = 0;
        if (delay_samples > max_delay) delay_samples = max_delay;

        for (int ch = 0; ch < channels; ch++) {
            int idx = i * channels + ch;
            int read_idx = (i + max_delay - delay_samples) * channels + ch;

            float delayed = (read_idx >= 0) ? delay_line[read_idx] : 0.0f;
            buf[idx] = buf[idx] * (1.0f - mix) + delayed * mix;
        }
    }

    free(delay_line);
}

/* ---- Master apply function ---------------------------------------------- */
/* Apply an audio effect to a buffer based on type and params */
void wb_audio_fx_process(float *buf, int n_frames, int channels, int sample_rate,
                          int fx_type, float *params) {
    if (!buf || !params) return;

    switch (fx_type) {
        case 0: /* EQ: params = [low_gain, mid_gain, high_gain] */
            wb_audio_fx_eq(buf, n_frames, channels, sample_rate,
                           params[0], params[1], params[2]);
            break;
        case 1: /* Reverb: params = [decay, mix] */
            wb_audio_fx_reverb(buf, n_frames, channels, sample_rate,
                               params[0], params[1]);
            break;
        case 2: /* Compressor: params = [threshold, ratio, attack, release] */
            wb_audio_fx_compressor(buf, n_frames, channels,
                                    params[0], params[1], params[2], params[3]);
            break;
        case 3: /* Delay: params = [time, feedback, mix] */
            wb_audio_fx_delay(buf, n_frames, channels, sample_rate,
                               params[0], params[1], params[2]);
            break;
        case 4: /* Distortion: params = [amount] */
            wb_audio_fx_distortion(buf, n_frames, channels, params[0]);
            break;
        case 5: /* Chorus: params = [rate, depth, mix] */
            wb_audio_fx_chorus(buf, n_frames, channels, sample_rate,
                                params[0], params[1], params[2]);
            break;
    }
}

/* ---- API functions for edit graph --------------------------------------- */

#include "wbus/wbus_edit.h"

int wb_edit_set_audio_fx(wb_edit_graph *g, int track, int clip_idx,
                          int fx_slot, const wb_audio_fx *fx) {
    if (!g || !fx) return -1;
    if (track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    if ((uint32_t)clip_idx >= tr->audio_clip_count) return -1;
    if (fx_slot < 0 || fx_slot >= WB_AUDIO_FX_PER_CLIP) return -1;

    /* If setting a VST3 FX, instantiate the plugin */
    void *vst3_inst = NULL;
    int vst3_param_count = 0;
    if (fx->type == WB_AUDIO_FX_VST3 && fx->vst3.plugin_name[0]) {
        /* Destroy old instance if replacing */
        if (tr->audio_clips[clip_idx].fx_chain[fx_slot].vst3.instance) {
            wb_vst3_destroy(tr->audio_clips[clip_idx].fx_chain[fx_slot].vst3.instance);
        }
        vst3_inst = wb_vst3_create(fx->vst3.plugin_name, (uint32_t)48000);
        vst3_param_count = wb_vst3_param_count(vst3_inst);
    }

    tr->audio_clips[clip_idx].fx_chain[fx_slot] = *fx;
    /* Set instance pointer after memcpy (overwritten by struct copy) */
    tr->audio_clips[clip_idx].fx_chain[fx_slot].vst3.instance = vst3_inst;
    tr->audio_clips[clip_idx].fx_chain[fx_slot].vst3.param_count = vst3_param_count;
    return 0;
}

int wb_edit_clear_audio_fx(wb_edit_graph *g, int track, int clip_idx,
                            int fx_slot) {
    if (!g) return -1;
    if (track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    if ((uint32_t)clip_idx >= tr->audio_clip_count) return -1;
    if (fx_slot < 0 || fx_slot >= WB_AUDIO_FX_PER_CLIP) return -1;

    /* If clearing a VST3 FX, destroy the plugin instance */
    if (tr->audio_clips[clip_idx].fx_chain[fx_slot].type == WB_AUDIO_FX_VST3 &&
        tr->audio_clips[clip_idx].fx_chain[fx_slot].vst3.instance) {
        wb_vst3_destroy(tr->audio_clips[clip_idx].fx_chain[fx_slot].vst3.instance);
    }

    memset(&tr->audio_clips[clip_idx].fx_chain[fx_slot], 0, sizeof(wb_audio_fx));
    return 0;
}

/* Apply all enabled FX in a clip's chain to a buffer */
void wb_edit_apply_audio_fx(wb_edit_graph *g, int track, int clip_idx,
                              float *buf, int n_frames, int channels,
                              int sample_rate) {
    if (!g || !buf) return;
    if (track < 0 || (uint32_t)track >= g->track_count) return;
    wb_edit_track *tr = &g->tracks[track];
    if ((uint32_t)clip_idx >= tr->audio_clip_count) return;

    wb_audio_fx *fx = tr->audio_clips[clip_idx].fx_chain;
    for (int i = 0; i < WB_AUDIO_FX_PER_CLIP; i++) {
        if (fx[i].enabled && fx[i].type < WB_AUDIO_FX_COUNT) {
            /* Convert structured params to flat array for processing */
            float params[8] = {0};
            switch (fx[i].type) {
                case WB_AUDIO_FX_EQ:
                    params[0] = powf(10.0f, fx[i].eq.low_gain / 20.0f);
                    params[1] = powf(10.0f, fx[i].eq.mid_gain / 20.0f);
                    params[2] = powf(10.0f, fx[i].eq.high_gain / 20.0f);
                    break;
                case WB_AUDIO_FX_REVERB:
                    params[0] = fx[i].reverb.room_size * 0.9f;
                    params[1] = fx[i].reverb.wet;
                    break;
                case WB_AUDIO_FX_COMPRESSOR:
                    params[0] = powf(10.0f, fx[i].compressor.threshold_db / 20.0f);
                    params[1] = fx[i].compressor.ratio;
                    params[2] = fx[i].compressor.attack_ms / 1000.0f;
                    params[3] = fx[i].compressor.release_ms / 1000.0f;
                    break;
                case WB_AUDIO_FX_DELAY:
                    params[0] = fx[i].delay.time_ms / 1000.0f;
                    params[1] = fx[i].delay.feedback;
                    params[2] = fx[i].delay.wet;
                    break;
                case WB_AUDIO_FX_DISTORTION:
                    params[0] = fx[i].distortion.drive;
                    break;
                case WB_AUDIO_FX_CHORUS:
                    params[0] = fx[i].chorus.rate_hz;
                    params[1] = fx[i].chorus.depth_ms / 1000.0f;
                    params[2] = fx[i].chorus.mix;
                    break;
                case WB_AUDIO_FX_VST3:
                    /* VST3: process via plugin instance, not float params */
                    if (fx[i].vst3.instance) {
                        /* Deinterleave stereo in/out */
                        /* For now, process as mono-mixed; full stereo VST3 needs deinterleave */
                        float *mix = (float *)malloc(n_frames * sizeof(float));
                        if (mix) {
                            for (int f = 0; f < n_frames; f++)
                                mix[f] = buf[f * channels] * 0.5f + (channels > 1 ? buf[f * channels + 1] * 0.5f : 0);
                            float *out = (float *)malloc(n_frames * sizeof(float));
                            if (out) {
                                wb_vst3_process(fx[i].vst3.instance, mix, mix, out, out, (uint32_t)n_frames);
                                /* Write back with mix */
                                float wet = fx[i].mix;
                                for (int f = 0; f < n_frames; f++) {
                                    float dry = buf[f * channels];
                                    buf[f * channels] = dry * (1.0f - wet) + out[f] * wet;
                                    if (channels > 1) {
                                        dry = buf[f * channels + 1];
                                        buf[f * channels + 1] = dry * (1.0f - wet) + out[f] * wet;
                                    }
                                }
                                free(out);
                            }
                            free(mix);
                        }
                    }
                    continue; /* skip the generic process() call below */
                default:
                    break;
            }
            wb_audio_fx_process(buf, n_frames, channels, sample_rate,
                                 (int)fx[i].type, params);
        }
    }
}
