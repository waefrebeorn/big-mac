/* wb_ytp_combo.c — YTP combination effects (R094f).
 *
 * Sex-O-Phone, Dance Rave, Meme Replacement, Paint Jobs,
 * Flip/Spin, Source Abuse, Subversion Poop, Scramble+Stutter.
 * Combines existing primitives into higher-level poopisms.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* Forward declarations of functions from wb_ytp_fx.c and wb_dark_arts.c */
extern void wb_swirl(uint8_t *dst, const uint8_t *src, int w, int h, float angle, float radius);
extern void wb_scramble(uint8_t *dst, const uint8_t *src, int w, int h, int seed, int block_size);
extern void wb_zoom_punch(uint8_t *dst, const uint8_t *src, int w, int h, float scale);
extern void wb_wave_displace(uint8_t *dst, const uint8_t *src, int w, int h,
                             float ax, float fx, float px, float ay, float fy, float py);
extern void wb_compression_torture(uint8_t *rgba, int w, int h, int quality);

/* ================================================================
 * FLIP / SPIN (animated rotation)
 * ================================================================ */

/* Animated flip: horizontal flip with smooth interpolation */
void wb_animated_flip(uint8_t *dst, const uint8_t *src, int w, int h,
                      float progress) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    /* progress: 0=no flip, 0.5=vertical squish, 1=fully flipped */
    float scale_x = cosf(progress * M_PI); /* 1 → 0 → -1 */

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float sx_f = (x - w * 0.5f) / scale_x + w * 0.5f;
            int sx = (int)sx_f;
            if (sx < 0) sx = 0;
            if (sx >= w) sx = w - 1;
            memcpy(dst + (y * w + x) * 4, src + (y * w + sx) * 4, 4);
        }
    }
}

/* Continuous spin: rotate frame by angle */
void wb_spin(uint8_t *dst, const uint8_t *src, int w, int h, float angle_deg) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    float cx = w * 0.5f, cy = h * 0.5f;
    float angle = angle_deg * M_PI / 180.0f;
    float cos_a = cosf(angle), sin_a = sinf(angle);

    memset(dst, 0, w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = x - cx, dy = y - cy;
            int sx = (int)(cx + dx * cos_a + dy * sin_a);
            int sy = (int)(cy - dx * sin_a + dy * cos_a);
            if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                memcpy(dst + (y * w + x) * 4, src + (sy * w + sx) * 4, 4);
            }
        }
    }
}

/* ================================================================
 * PAINT JOBS (MS Paint style)
 * ================================================================ */

/* Crude line drawing (Bresenham) */
void wb_paint_line(uint8_t *rgba, int w, int h,
                   int x0, int y0, int x1, int y1,
                   uint8_t r, uint8_t g, uint8_t b, int thickness) {
    if (!rgba || w <= 0 || h <= 0) return;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        for (int tx = -thickness/2; tx <= thickness/2; tx++) {
            for (int ty = -thickness/2; ty <= thickness/2; ty++) {
                int px = x0 + tx, py = y0 + ty;
                if (px >= 0 && px < w && py >= 0 && py < h) {
                    int idx = (py * w + px) * 4;
                    rgba[idx] = r; rgba[idx+1] = g; rgba[idx+2] = b; rgba[idx+3] = 255;
                }
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Crude filled circle */
void wb_paint_circle(uint8_t *rgba, int w, int h,
                     int cx, int cy, int radius,
                     uint8_t r, uint8_t g, uint8_t b, int filled) {
    if (!rgba || w <= 0 || h <= 0) return;
    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            if (x < 0 || x >= w || y < 0 || y >= h) continue;
            int dist = (x-cx)*(x-cx) + (y-cy)*(y-cy);
            if (filled ? dist <= radius*radius : dist <= radius*radius + radius &&
                                                      dist >= radius*radius - radius) {
                int idx = (y * w + x) * 4;
                rgba[idx] = r; idx++;
                rgba[idx] = g; idx++;
                rgba[idx] = b; idx++;
                rgba[idx] = 255;
            }
        }
    }
}

/* MS Paint style arrow */
void wb_paint_arrow(uint8_t *rgba, int w, int h,
                    int x0, int y0, int x1, int y1,
                    uint8_t r, uint8_t g, uint8_t b) {
    wb_paint_line(rgba, w, h, x0, y0, x1, y1, r, g, b, 3);
    /* Arrowhead */
    float angle = atan2f((float)(y1 - y0), (float)(x1 - x0));
    int a_len = 15;
    float a_angle = 0.5f;
    wb_paint_line(rgba, w, h, x1, y1,
                  (int)(x1 - a_len * cosf(angle - a_angle)),
                  (int)(y1 - a_len * sinf(angle - a_angle)), r, g, b, 3);
    wb_paint_line(rgba, w, h, x1, y1,
                  (int)(x1 - a_len * cosf(angle + a_angle)),
                  (int)(y1 - a_len * sinf(angle + a_angle)), r, g, b, 3);
}

/* ================================================================
 * MEME REPLACEMENT (replace segment with meme audio)
 * ================================================================ */

/* Overlay a meme soundbite onto an audio buffer at a specific time */
int wb_meme_replace(float *audio, int n_frames, int n_channels,
                     float sample_rate, int insert_frame,
                     const float *meme_samples, int meme_frames) {
    if (!audio || !meme_samples || meme_frames <= 0) return -1;
    if (insert_frame < 0 || insert_frame >= n_frames) return -1;

    int copy_frames = meme_frames;
    if (insert_frame + copy_frames > n_frames)
        copy_frames = n_frames - insert_frame;

    /* Crossfade edges to avoid clicks */
    int fade_len = (int)(sample_rate * 0.01f); /* 10ms fade */
    if (fade_len > copy_frames / 2) fade_len = copy_frames / 2;

    for (int i = 0; i < copy_frames; i++) {
        float env = 1.0f;
        if (i < fade_len)
            env = (float)i / fade_len;
        else if (i >= copy_frames - fade_len)
            env = (float)(copy_frames - i) / fade_len;

        for (int c = 0; c < n_channels; c++) {
            int src_c = c < n_channels ? c : 0;
            float meme_val = meme_samples[i * n_channels + src_c] * env;
            audio[(insert_frame + i) * n_channels + c] += meme_val;
            /* Soft clip */
            if (audio[(insert_frame + i) * n_channels + c] > 1.0f)
                audio[(insert_frame + i) * n_channels + c] = 1.0f;
            if (audio[(insert_frame + i) * n_channels + c] < -1.0f)
                audio[(insert_frame + i) * n_channels + c] = -1.0f;
        }
    }
    return copy_frames;
}

/* ================================================================
 * SCRAMBLE + STUTTER COMBO
 * ================================================================ */

/* Scramble-stutter: scramble blocks then stutter the result */
void wb_scramble_stutter(uint8_t *dst, const uint8_t *src, int w, int h,
                          int seed, int block_size, int n_repeats) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    int cols = w / block_size;
    int rows = h / block_size;
    int n_blocks = cols * rows;
    if (n_blocks <= 0) return;

    /* Generate permutation */
    srand(seed);
    int *perm = (int *)malloc(n_blocks * sizeof(int));
    if (!perm) return;
    for (int i = 0; i < n_blocks; i++) perm[i] = i;
    for (int i = n_blocks - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
    }

    int frame_size = w * h * 4;

    /* For each repeat, copy scrambled frame */
    for (int rep = 0; rep < n_repeats; rep++) {
        uint8_t *frame_dst = dst + rep * frame_size;

        /* Copy blocks in permuted order */
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int dst_idx = r * cols + c;
                int src_idx = perm[dst_idx % n_blocks];
                int src_r = src_idx / cols;
                int src_c = src_idx % cols;

                for (int by = 0; by < block_size; by++) {
                    for (int bx = 0; bx < block_size; bx++) {
                        int sx = src_c * block_size + bx;
                        int sy = src_r * block_size + by;
                        int dx = c * block_size + bx;
                        int dy = r * block_size + by;
                        if (sx < w && sy < h && dx < w && dy < h) {
                            memcpy(frame_dst + (dy * w + dx) * 4,
                                   src + (sy * w + sx) * 4, 4);
                        }
                    }
                }
            }
        }
    }
    free(perm);
}

/* ================================================================
 * SOURCE ABUSE (push single source to absurdity)
 * ================================================================ */

/* Apply a chain of effects to abuse a single source frame */
void wb_source_abuse(uint8_t *dst, const uint8_t *src, int w, int h,
                     int iteration, int seed) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    uint8_t *temp = (uint8_t *)malloc(w * h * 4);
    if (!temp) return;

    memcpy(temp, src, w * h * 4);

    /* Each iteration applies random effects */
    srand(seed + iteration);
    int n_effects = 3 + (iteration % 5);

    for (int i = 0; i < n_effects; i++) {
        int effect = rand() % 6;
        switch (effect) {
            case 0: {
                float angle = (float)(rand() % 360);
                wb_spin(dst, temp, w, h, angle);
                memcpy(temp, dst, w * h * 4);
                break;
            }
            case 1: {
                float strength = (float)(rand() % 100) / 100.0f;
                wb_swirl(dst, temp, w, h, strength * 180.0f, 0.5f);
                memcpy(temp, dst, w * h * 4);
                break;
            }
            case 2: {
                wb_compression_torture(temp, w, h, 20 + rand() % 30);
                break;
            }
            case 3: {
                int bs = 4 + rand() % 16;
                wb_scramble(dst, temp, w, h, rand(), bs);
                memcpy(temp, dst, w * h * 4);
                break;
            }
            case 4: {
                float scale = 1.0f + (float)(rand() % 50) / 100.0f;
                wb_zoom_punch(dst, temp, w, h, scale);
                memcpy(temp, dst, w * h * 4);
                break;
            }
            case 5: {
                float amp = (float)(rand() % 10);
                float freq = 2.0f + (float)(rand() % 8);
                wb_wave_displace(dst, temp, w, h, amp, freq, 0.0f, amp * 0.5f, freq, M_PI * 0.5f);
                memcpy(temp, dst, w * h * 4);
                break;
            }
        }
    }

    memcpy(dst, temp, w * h * 4);
    free(temp);
}

/* ================================================================
 * SUBVERSION POOP (subvert expectations)
 * ================================================================ */

/* Calm → chaos: start normal, gradually increase distortion */
void wb_subversion_poop(uint8_t *dst, const uint8_t *src, int w, int h,
                        float chaos, int seed) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    if (chaos <= 0.0f) { memcpy(dst, src, w * h * 4); return; }
    if (chaos > 1.0f) chaos = 1.0f;

    memcpy(dst, src, w * h * 4);

    /* Apply increasing layers of distortion based on chaos level */
    if (chaos > 0.2f) {
        /* Slight color shift */
        for (int i = 0; i < w * h; i++) {
            dst[i*4] = (uint8_t)(dst[i*4] * (1.0f + chaos * 0.1f));
            if (dst[i*4] > 255) dst[i*4] = 255;
        }
    }
    if (chaos > 0.4f) {
        /* Wave distortion */
        uint8_t *temp = (uint8_t *)malloc(w * h * 4);
        if (!temp) return;
        float amp = chaos * 8.0f;
        wb_wave_displace(temp, dst, w, h, amp, 3.0f, 0.0f, amp * 0.7f, 2.5f, M_PI * 0.3f);
        memcpy(dst, temp, w * h * 4);
        free(temp);
    }
    if (chaos > 0.6f) {
        /* Scramble */
        uint8_t *temp = (uint8_t *)malloc(w * h * 4);
        if (!temp) return;
        int bs = (int)(4 + chaos * 12);
        wb_scramble(temp, dst, w, h, seed, bs);
        memcpy(dst, temp, w * h * 4);
        free(temp);
    }
    if (chaos > 0.8f) {
        /* Full datamosh + compression torture */
        wb_compression_torture(dst, w, h, 25 + (int)(chaos * 10));
    }
}
