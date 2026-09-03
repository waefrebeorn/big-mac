/* wb_dark_arts.c — YTP dark arts effects (R094c).
 *
 * Compression torture, infinite loop, Stare Down/Mysterious Zoom,
 * Bleep Censor, MLG montage elements, Saponite.
 * Pure C11 pixel-level + ffmpeg pipeline builder.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ================================================================
 * COMPRESSION TORTURE
 * ================================================================ */

/* Simulate heavy JPEG compression by quantizing color channels */
void wb_compression_torture(uint8_t *rgba, int w, int h, int quality) {
    if (!rgba || w <= 0 || h <= 0) return;
    /* quality: 0=maximum torture, 255=no effect */
    if (quality < 0) quality = 0;
    if (quality > 255) quality = 255;

    int levels = (quality * 255) / 100; /* map to color levels */
    if (levels < 2) levels = 2;
    if (levels > 255) levels = 255;

    float step = 255.0f / (levels - 1);

    for (int i = 0; i < w * h; i++) {
        /* Quantize RGB, leave alpha */
        for (int c = 0; c < 3; c++) {
            float val = rgba[i * 4 + c];
            int q = (int)(val / step + 0.5f);
            if (q >= levels) q = levels - 1;
            rgba[i * 4 + c] = (uint8_t)(q * step);
        }
    }
}

/* ================================================================
 * STARE DOWN / MYSTERIOUS ZOOM
 * ================================================================ */

/* Stare Down: freeze frame with slow zoom in on a face/area */
void wb_stare_down(uint8_t *dst, const uint8_t *src, int w, int h,
                   float zoom_level, float center_x, float center_y) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    float cx = center_x * w;
    float cy = center_y * h;
    float scale = 1.0f + zoom_level;

    memset(dst, 0, w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = (int)(cx + (x - cx) / scale);
            int sy = (int)(cy + (y - cy) / scale);
            if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                memcpy(dst + (y * w + x) * 4, src + (sy * w + sx) * 4, 4);
            }
        }
    }
}

/* Mysterious Zoom: continuous slow zoom with slight rotation */
void wb_mysterious_zoom(uint8_t *dst, const uint8_t *src, int w, int h,
                        float zoom, float angle_deg, float cx_norm, float cy_norm) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    float cx = cx_norm * w;
    float cy = cy_norm * h;
    float angle = angle_deg * M_PI / 180.0f;
    float cos_a = cosf(angle), sin_a = sinf(angle);
    float scale = 1.0f + zoom;

    memset(dst, 0, w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = (x - cx) / scale;
            float dy = (y - cy) / scale;
            int sx = (int)(cx + dx * cos_a + dy * sin_a);
            int sy = (int)(cy - dx * sin_a + dy * cos_a);
            if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                memcpy(dst + (y * w + x) * 4, src + (sy * w + sx) * 4, 4);
            }
        }
    }
}

/* ================================================================
 * BLEEP CENSOR
 * ================================================================ */

/* Replace a rectangular region with a solid color (censor bar) */
void wb_bleep_bar(uint8_t *rgba, int w, int h,
                  int x0, int y0, int x1, int y1,
                  uint8_t r, uint8_t g, uint8_t b) {
    if (!rgba || w <= 0 || h <= 0) return;
    if (x0 < 0) x0 = 0; if (x1 > w) x1 = w;
    if (y0 < 0) y0 = 0; if (y1 > h) y1 = h;

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            int idx = (y * w + x) * 4;
            rgba[idx + 0] = r;
            rgba[idx + 1] = g;
            rgba[idx + 2] = b;
            rgba[idx + 3] = 255;
        }
    }
}

/* ================================================================
 * MLG MONTAGE ELEMENTS
 * ================================================================ */

/* MLG flash: brief white flash + red tint */
void wb_mlg_flash(uint8_t *rgba, int w, int h, float intensity) {
    if (!rgba || w <= 0 || h <= 0) return;
    for (int i = 0; i < w * h; i++) {
        float r = rgba[i*4+0] / 255.0f;
        float g = rgba[i*4+1] / 255.0f;
        float b = rgba[i*4+2] / 255.0f;
        /* Flash white */
        r = r + (1.0f - r) * intensity;
        g = g + (1.0f - g) * intensity;
        /* Red tint */
        r = r + intensity * 0.3f;
        if (r > 1) r = 1;
        if (g > 1) g = 1;
        if (b > 1) b = 1;
        rgba[i*4+0] = (uint8_t)(r * 255);
        rgba[i*4+1] = (uint8_t)(g * 255);
        rgba[i*4+2] = (uint8_t)(b * 255);
    }
}

/* ================================================================
 * SAPONITE (smooth zoom + reverb visualization)
 * ================================================================ */

/* Saponite: smooth slow zoom with color grading */
void wb_saponite(uint8_t *dst, const uint8_t *src, int w, int h,
                 float zoom, float saturation_boost) {
    if (!dst || !src || w <= 0 || h <= 0) return;
    float cx = w * 0.5f, cy = h * 0.5f;
    float scale = 1.0f + zoom;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = (int)(cx + (x - cx) / scale);
            int sy = (int)(cy + (y - cy) / scale);
            if (sx < 0 || sx >= w || sy < 0 || sy >= h) continue;

            float r = src[(sy * w + sx) * 4 + 0] / 255.0f;
            float g = src[(sy * w + sx) * 4 + 1] / 255.0f;
            float b = src[(sy * w + sx) * 4 + 2] / 255.0f;

            /* Saturation boost */
            float gray = 0.299f * r + 0.587f * g + 0.114f * b;
            r = gray + (r - gray) * (1.0f + saturation_boost);
            g = gray + (g - gray) * (1.0f + saturation_boost);
            b = gray + (b - gray) * (1.0f + saturation_boost);

            if (r < 0) r = 0; if (r > 1) r = 1;
            if (g < 0) g = 0; if (g > 1) g = 1;
            if (b < 0) b = 0; if (b > 1) b = 1;

            int idx = (y * w + x) * 4;
            dst[idx + 0] = (uint8_t)(r * 255);
            dst[idx + 1] = (uint8_t)(g * 255);
            dst[idx + 2] = (uint8_t)(b * 255);
            dst[idx + 3] = 255;
        }
    }
}

/* ================================================================
 * INFINITE LOOP (seamless loop helper)
 * ================================================================ */

/* Crossfade last N frames into first N frames for seamless looping */
void wb_infinite_loop_blend(uint8_t *frames, int w, int h,
                            int n_frames, int blend_frames) {
    if (!frames || w <= 0 || h <= 0 || n_frames < 2) return;
    if (blend_frames <= 0) return;
    if (blend_frames > n_frames / 2) blend_frames = n_frames / 2;

    int frame_size = w * h * 4;

    for (int i = 0; i < blend_frames; i++) {
        float t = (float)i / blend_frames;
        uint8_t *last = frames + (n_frames - blend_frames + i) * frame_size;
        uint8_t *first = frames + i * frame_size;

        for (int p = 0; p < frame_size; p++) {
            float a = last[p] / 255.0f;
            float b = first[p] / 255.0f;
            float blended = a * (1.0f - t) + b * t;
            first[p] = (uint8_t)(blended * 255.0f);
        }
    }
}

/* ================================================================
 * MAD DASH CUT (rapid accelerating cuts)
 * ================================================================ */

/* Calculate cut positions for mad dash effect */
/* Returns array of frame indices where cuts happen */
/* Mad dash: cuts start slow and accelerate (gaps decrease over time) */
int wb_mad_dash_cuts(int total_frames, int n_cuts, int *cut_positions) {
    if (!cut_positions || n_cuts <= 0 || total_frames <= 0) return 0;

    /* Start with large gaps, decrease them */
    float pos = 0;
    float gap = (float)total_frames / n_cuts * 2.0f; /* start wide */
    float min_gap = 2.0f;
    float decay = 0.7f; /* gap shrinks by 30% each cut */

    for (int i = 0; i < n_cuts && pos < total_frames; i++) {
        cut_positions[i] = (int)pos;
        pos += gap;
        gap *= decay;
        if (gap < min_gap) gap = min_gap;
    }

    /* Count valid cuts */
    int count = 0;
    for (int i = 0; i < n_cuts; i++) {
        if (cut_positions[i] < total_frames) count++;
        else break;
    }
    return count;
}
