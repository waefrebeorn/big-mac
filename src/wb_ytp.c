/* wb_ytp.c — YouTube Poop / meme editing engine (R077 Phase 5).
 *
 * YTP poopisms and meme editing techniques:
 * stutter loop, sentence mixing, pitch shift, ear-rape,
 * reverse, speed up/slow down, word salad, deep fry, VHS, glitch.
 *
 * Works with wb_vfx.c for visual effects and the audio engine.
 * Pure C11, no third party.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ===================================================================
 * Stutter Loop
 * =================================================================== */

/* Repeat a segment of audio N times.
 * in: input samples (n_channels interleaved)
 * out: output buffer (must be large enough: segment_len * n_repeat * n_channels)
 */
int wb_stutter_loop(const float *in, float *out, int segment_frames,
                     int n_channels, int n_repeat) {
    int segment_samples = segment_frames * n_channels;
    for (int r = 0; r < n_repeat; r++) {
        memcpy(out + r * segment_samples, in, segment_samples * sizeof(float));
    }
    return n_repeat * segment_frames;
}

/* Stutter loop with effect per iteration (stutter loop plus) */
int wb_stutter_loop_plus(const float *in, float *out, int segment_frames,
                          int n_channels, int n_repeat,
                          float (*effect)(float, int, void*), void *ctx) {
    int segment_samples = segment_frames * n_channels;
    /* First copy: original */
    memcpy(out, in, segment_samples * sizeof(float));

    for (int r = 1; r < n_repeat; r++) {
        float *dst = out + r * segment_samples;
        for (int i = 0; i < segment_samples; i++) {
            dst[i] = effect(in[i], r, ctx);
        }
    }
    return n_repeat * segment_frames;
}

/* Example effect: gain boost per iteration */
float wb_stutter_effect_gain(float sample, int iteration, void *ctx) {
    float boost = *(float*)ctx;
    return sample * powf(boost, (float)iteration);
}

/* ===================================================================
 * Pitch Shift (phase vocoder, simplified)
 * =================================================================== */

/* Simple pitch shift via resampling + linear interpolation.
 * ratio > 1 = higher pitch (chipmunk), < 1 = lower pitch (demon).
 * Uses time-domain PSOLA-like approach.
 */
int wb_pitch_shift(const float *in, float *out, int n_frames, int n_channels,
                    float ratio) {
    if (ratio <= 0.1f) ratio = 0.1f;
    if (ratio > 4.0f) ratio = 4.0f;

    int out_frames = (int)(n_frames / ratio);

    for (int i = 0; i < out_frames; i++) {
        float src_pos = (float)i * ratio;
        int src_idx = (int)src_pos;
        float frac = src_pos - src_idx;

        if (src_idx + 1 >= n_frames) {
            /* Last frame: just copy */
            for (int c = 0; c < n_channels; c++) {
                out[i * n_channels + c] = in[(n_frames - 1) * n_channels + c];
            }
        } else {
            for (int c = 0; c < n_channels; c++) {
                float a = in[src_idx * n_channels + c];
                float b = in[(src_idx + 1) * n_channels + c];
                out[i * n_channels + c] = a * (1.0f - frac) + b * frac;
            }
        }
    }

    return out_frames;
}

/* ===================================================================
 * Ear-Rape (volume spike)
 * =================================================================== */

void wb_earrape(float *samples, int count, float multiplier) {
    for (int i = 0; i < count; i++) {
        samples[i] *= multiplier;
        /* Hard clip to prevent overflow */
        if (samples[i] > 1.0f) samples[i] = 1.0f;
        if (samples[i] < -1.0f) samples[i] = -1.0f;
    }
}

/* ===================================================================
 * Reverse
 * =================================================================== */

void wb_reverse(float *samples, int n_frames, int n_channels) {
    for (int f = 0; f < n_frames / 2; f++) {
        int opp = n_frames - 1 - f;
        for (int c = 0; c < n_channels; c++) {
            float tmp = samples[f * n_channels + c];
            samples[f * n_channels + c] = samples[opp * n_channels + c];
            samples[opp * n_channels + c] = tmp;
        }
    }
}

/* ===================================================================
 * Speed Control (without pitch change)
 * =================================================================== */

/* Time-stretch via overlap-add (WSOLA-like, simplified).
 * ratio > 1 = faster, < 1 = slower.
 */
int wb_time_stretch(const float *in, float *out, int n_frames, int n_channels,
                      float ratio) {
    if (ratio <= 0.1f) ratio = 0.1f;
    if (ratio > 4.0f) ratio = 4.0f;

    int out_frames = (int)(n_frames / ratio);
    int window = 1024;  /* analysis window */
    int hop_out = (int)(window / ratio / 2);

    if (hop_out < 1) hop_out = 1;
    memset(out, 0, out_frames * n_channels * sizeof(float));

    int pos_in = 0;
    int pos_out = 0;

    while (pos_out + window <= out_frames && pos_in + window <= n_frames) {
        /* Copy window from input to output */
        for (int i = 0; i < window && pos_out + i < out_frames; i++) {
            for (int c = 0; c < n_channels; c++) {
                if (pos_in + i < n_frames) {
                    out[(pos_out + i) * n_channels + c] +=
                        in[(pos_in + i) * n_channels + c];
                }
            }
        }
        pos_in += hop_out;
        pos_out += hop_out;
    }

    return out_frames;
}

/* ===================================================================
 * Sentence Mixing Helper
 * =================================================================== */

/* Splice audio at phoneme boundaries (simplified: at zero crossings).
 * Returns array of splice points (frame indices).
 */
int wb_find_splice_points(const float *samples, int n_frames, int n_channels,
                           int *points, int max_points) {
    if (!samples || !points || n_frames < 2) return 0;

    int n_points = 0;
    int min_gap = n_frames / 50;  /* min 2% of total length between splices */

    for (int i = 1; i < n_frames - 1 && n_points < max_points; i++) {
        float prev = samples[(i-1) * n_channels];
        float curr = samples[i * n_channels];

        /* Zero crossing */
        if ((prev <= 0 && curr > 0) || (prev >= 0 && curr < 0)) {
            /* Check if energy is low (between words) */
            float energy = 0;
            int window = 50;
            for (int j = i - window/2; j < i + window/2 && j >= 0 && j < n_frames; j++) {
                float s = samples[j * n_channels];
                energy += s * s;
            }
            energy /= window;

            if (energy < 0.001f && (n_points == 0 || i - points[n_points-1] > min_gap)) {
                points[n_points++] = i;
            }
        }
    }

    return n_points;
}

/* Rearrange segments to form new "sentences".
 * segments: array of {start_frame, end_frame} to rearrange
 * order: array of segment indices in desired order
 */
int wb_sentence_mix(const float *in, float *out,
                     int n_channels, int n_output_frames,
                     const int *segments,  /* start/end pairs: [s0_start, s0_end, s1_start, ...] */
                     const int *order, int n_segments) {
    int pos_out = 0;
    int pos_samples = 0;

    for (int i = 0; i < n_segments; i++) {
        int seg_idx = order[i];
        int start = segments[seg_idx * 2];
        int end = segments[seg_idx * 2 + 1];
        int len = end - start;
        int len_samples = len * n_channels;

        if (pos_samples + len_samples > n_output_frames * n_channels) break;

        memcpy(out + pos_samples, in + start * n_channels, len_samples * sizeof(float));
        pos_samples += len_samples;
        pos_out += len;
    }

    return pos_out;
}

/* ===================================================================
 * Word Salad (random word rearrangement)
 * =================================================================== */

void wb_word_salad(int *words, int n_words) {
    /* Fisher-Yates shuffle */
    for (int i = n_words - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = words[i];
        words[i] = words[j];
        words[j] = tmp;
    }
}

/* ===================================================================
 * Datamosh Prep (I-frame removal simulation)
 * =================================================================== */

/* Simulate datamosh by copying motion from one frame region to another.
 * This creates the "melting" effect by displacing pixels based on
 * difference between two frames.
 */
void wb_datamosh(uint8_t *output, const uint8_t *frame_a, const uint8_t *frame_b,
                  int width, int height, float intensity) {
    memcpy(output, frame_a, width * height * 4);

    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            int idx = (y * width + x) * 4;

            /* Compute local motion: difference between frames */
            int dr = (int)frame_b[idx+0] - (int)frame_a[idx+0];
            int dg = (int)frame_b[idx+1] - (int)frame_a[idx+1];
            int db = (int)frame_b[idx+2] - (int)frame_a[idx+2];

            float motion = sqrtf((float)(dr*dr + dg*dg + db*db)) / 441.67f;  /* normalize to 0..1 */

            if (motion > 0.1f) {
                /* Displace pixel by motion vector */
                int offset_x = (int)(dr * intensity * 0.1f);
                int offset_y = (int)(dg * intensity * 0.1f);

                int src_x = x + offset_x;
                int src_y = y + offset_y;

                if (src_x >= 0 && src_x < width && src_y >= 0 && src_y < height) {
                    int src_idx = (src_y * width + src_x) * 4;
                    output[idx+0] = frame_a[src_idx+0];
                    output[idx+1] = frame_a[src_idx+1];
                    output[idx+2] = frame_a[src_idx+2];
                }
            }
        }
    }
}

/* ===================================================================
 * Spadinner (insert famous soundbites)
 * =================================================================== */

/* Trigger a soundbite at a specific time. Returns the soundbite ID.
 * In practice, this loads a pre-baked audio clip and inserts it.
 */
typedef struct {
    float *samples;
    int n_frames;
    int n_channels;
    float sample_rate;
    char name[32];
} wb_soundbite;

int wb_insert_soundbite(float *audio, int n_frames, int n_channels,
                          float sample_rate, int insert_frame,
                          const wb_soundbite *bite) {
    (void)sample_rate;
    if (!audio || !bite || !bite->samples) return -1;

    int copy_frames = bite->n_frames;
    if (insert_frame + copy_frames > n_frames) {
        copy_frames = n_frames - insert_frame;
    }
    if (copy_frames <= 0) return -1;

    /* Mix soundbite into audio (additive) */
    for (int i = 0; i < copy_frames; i++) {
        for (int c = 0; c < n_channels; c++) {
            float s = 0;
            if (c < bite->n_channels) {
                s = bite->samples[i * bite->n_channels + c];
            }
            audio[(insert_frame + i) * n_channels + c] += s;
        }
    }

    return copy_frames;
}

/* ===================================================================
 * Vine Boom (impact effect)
 * =================================================================== */

/* Generate a vine boom sound (low thump + noise burst). */
void wb_ytp_vine_boom(float *out, int frames, int sample_rate) {
    (void)sample_rate;
    int thump_len = (int)(sample_rate * 0.1f);  /* 100ms thump */

    for (int i = 0; i < frames; i++) {
        float s = 0;
        if (i < thump_len) {
            float t = (float)i / sample_rate;
            /* Low frequency thump: 80Hz sine with fast decay */
            float env = expf(-t * 30.0f);
            s = sinf(t * 2.0f * M_PI * 80.0f) * env * 0.8f;

            /* Add noise burst at start */
            if (i < thump_len / 4) {
                float noise = ((float)rand() / RAND_MAX - 0.5f) * 2.0f;
                s += noise * expf(-t * 60.0f) * 0.5f;
            }
        }

        out[i] = s;
    }
}
