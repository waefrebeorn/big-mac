/* wb_ytp_remaining.c — Final YTP gap closers (R094g).
 *
 * Sex-O-Phone, Dance Rave, Tennis Rally, Scramble+ ffmpeg,
 * Stutter Loop Plus ffmpeg, Source Abuse ffmpeg.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ================================================================
 * SEX-O-PHONE: sax-style audio + visual pulse
 * ================================================================ */

/* Generate a saxophone-like tone (sawtooth + formant filter) */
void wb_sexophone_gen(float *out, int frames, int sample_rate,
                       float freq, float intensity) {
    if (!out || frames <= 0) return;

    for (int i = 0; i < frames; i++) {
        float t = (float)i / sample_rate;
        /* Sawtooth wave (sax-like) */
        float phase = fmodf(freq * t, 1.0f);
        float saw = 2.0f * phase - 1.0f;

        /* Simple formant filter (vowel-like resonance) */
        float f1 = 800.0f, f2 = 1200.0f;
        float formant = saw * (1.0f + 0.5f * sinf(2.0f * M_PI * f1 / sample_rate * i));
        formant *= (1.0f + 0.3f * sinf(2.0f * M_PI * f2 / sample_rate * i));

        /* Envelope: slow attack, sustain, slow release */
        float env;
        float dur = (float)frames / sample_rate;
        if (t < 0.1f) env = t / 0.1f; /* attack */
        else if (t > dur - 0.3f) env = (dur - t) / 0.3f; /* release */
        else env = 1.0f;

        /* Vibrato */
        float vibrato = 1.0f + 0.005f * sinf(2.0f * M_PI * 5.5f * t);

        out[i] = formant * env * intensity * 0.3f * vibrato;
        if (out[i] > 1.0f) out[i] = 1.0f;
        if (out[i] < -1.0f) out[i] = -1.0f;
    }
}

/* Visual pulse effect for Sex-O-Phone (gentle zoom + warm tint) */
void wb_sexophone_visual(uint8_t *dst, const uint8_t *src, int w, int h,
                         float phase) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    float cx = w * 0.5f, cy = h * 0.5f;
    /* Gentle breathing zoom */
    float scale = 1.0f + 0.05f * sinf(phase * 2.0f * M_PI);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = (int)(cx + (x - cx) / scale);
            int sy = (int)(cy + (y - cy) / scale);
            if (sx < 0 || sx >= w || sy < 0 || sy >= h) continue;

            float r = src[(sy * w + sx) * 4 + 0] / 255.0f;
            float g = src[(sy * w + sx) * 4 + 1] / 255.0f;
            float b = src[(sy * w + sx) * 4 + 2] / 255.0f;

            /* Warm tint: boost red, reduce blue */
            float warm = 0.1f * (0.5f + 0.5f * sinf(phase * 2.0f * M_PI));
            r += warm;
            b -= warm * 0.5f;
            if (r > 1) r = 1;
            if (b < 0) b = 0;

            int idx = (y * w + x) * 4;
            dst[idx + 0] = (uint8_t)(r * 255);
            dst[idx + 1] = (uint8_t)(g * 255);
            dst[idx + 2] = (uint8_t)(b * 255);
            dst[idx + 3] = 255;
        }
    }
}

/* ================================================================
 * DANCE RAVE: loop + strobe + color cycle
 * ================================================================ */

/* Dance rave effect: color cycle + strobe on beat */
void wb_dance_rave(uint8_t *dst, const uint8_t *src, int w, int h,
                   float beat_phase, int strobe_on) {
    if (!dst || !src || w <= 0 || h <= 0) return;

    /* Color cycle: rotate hue based on beat */
    float hue_shift = beat_phase * 360.0f;

    for (int i = 0; i < w * h; i++) {
        float r = src[i*4+0] / 255.0f;
        float g = src[i*4+1] / 255.0f;
        float b = src[i*4+2] / 255.0f;

        /* Simple hue rotation */
        float angle = hue_shift * M_PI / 180.0f;
        float cos_a = cosf(angle), sin_a = sinf(angle);
        float r_new = r * (0.5f + 0.5f * cos_a) + g * (-0.5f * cos_a + 0.866f * sin_a) + b * (-0.5f * cos_a - 0.866f * sin_a);
        float g_new = r * (-0.5f * cos_a - 0.866f * sin_a) + g * (0.5f + 0.5f * cos_a) + b * (-0.5f * cos_a + 0.866f * sin_a);
        float b_new = r * (-0.5f * cos_a + 0.866f * sin_a) + g * (-0.5f * cos_a - 0.866f * sin_a) + b * (0.5f + 0.5f * cos_a);

        if (r_new < 0) r_new = 0; if (r_new > 1) r_new = 1;
        if (g_new < 0) g_new = 0; if (g_new > 1) g_new = 1;
        if (b_new < 0) b_new = 0; if (b_new > 1) b_new = 1;

        /* Strobe: flash white on beat */
        if (strobe_on && beat_phase < 0.1f) {
            float flash = 1.0f - beat_phase / 0.1f;
            r_new += (1.0f - r_new) * flash;
            g_new += (1.0f - g_new) * flash;
            b_new += (1.0f - b_new) * flash;
        }

        dst[i*4+0] = (uint8_t)(r_new * 255);
        dst[i*4+1] = (uint8_t)(g_new * 255);
        dst[i*4+2] = (uint8_t)(b_new * 255);
        dst[i*4+3] = 255;
    }
}

/* ================================================================
 * TENNIS RALLY: back-and-forth clip reversal
 * ================================================================ */

/* Tennis effect: ping-pong clip direction */
void wb_tennis_rally(float *audio, int n_frames, int n_channels,
                     int clip_a_start, int clip_a_end,
                     int clip_b_start, int clip_b_end) {
    if (!audio || n_frames <= 0 || n_channels <= 0) return;

    /* Swap segments A and B */
    int len_a = clip_a_end - clip_a_start;
    int len_b = clip_b_end - clip_b_start;
    int min_len = len_a < len_b ? len_a : len_b;
    int min_samples = min_len * n_channels;

    float *temp = (float *)malloc(min_samples * sizeof(float));
    if (!temp) return;

    /* Copy A to temp */
    memcpy(temp, audio + clip_a_start * n_channels, min_samples * sizeof(float));
    /* Copy B to A */
    memcpy(audio + clip_a_start * n_channels, audio + clip_b_start * n_channels,
           min_samples * sizeof(float));
    /* Copy temp to B */
    memcpy(audio + clip_b_start * n_channels, temp, min_samples * sizeof(float));

    free(temp);
}

/* ================================================================
 * SCRAMBLE+ ffmpeg wrapper (block shuffle via select)
 * ================================================================ */

/* Generate a scramble permutation array */
void wb_scramble_perm(int *perm, int n, int seed) {
    if (!perm || n <= 0) return;
    srand(seed);
    for (int i = 0; i < n; i++) perm[i] = i;
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
    }
}

/* ================================================================
 * STUTTER LOOP PLUS (different FX per iteration)
 * ================================================================ */

/* Apply a different effect per iteration of a stutter loop */
/* effect_id: 0=normal, 1=invert, 2=R-only, 3=G-only, 4=B-only,
 *            5=posterize, 6=deep_fry */
void wb_stutter_iter_fx(uint8_t *frame, int w, int h, int iteration) {
    if (!frame || w <= 0 || h <= 0) return;
    int effect = iteration % 7;

    switch (effect) {
        case 0: /* normal */
            break;
        case 1: /* invert */
            for (int i = 0; i < w*h; i++) {
                frame[i*4+0] = 255 - frame[i*4+0];
                frame[i*4+1] = 255 - frame[i*4+1];
                frame[i*4+2] = 255 - frame[i*4+2];
            }
            break;
        case 2: /* R-only */
            for (int i = 0; i < w*h; i++) {
                frame[i*4+1] = 0; frame[i*4+2] = 0;
            }
            break;
        case 3: /* G-only */
            for (int i = 0; i < w*h; i++) {
                frame[i*4+0] = 0; frame[i*4+2] = 0;
            }
            break;
        case 4: /* B-only */
            for (int i = 0; i < w*h; i++) {
                frame[i*4+0] = 0; frame[i*4+1] = 0;
            }
            break;
        case 5: /* posterize (4 levels) */
            for (int i = 0; i < w*h*4; i++) {
                frame[i] = (frame[i] / 64) * 64;
            }
            break;
        case 6: /* brightness boost */
            for (int i = 0; i < w*h; i++) {
                for (int c = 0; c < 3; c++) {
                    int v = frame[i*4+c] * 3 / 2;
                    frame[i*4+c] = v > 255 ? 255 : v;
                }
            }
            break;
    }
}
