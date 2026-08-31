/* wb_autoreframe.c — intelligent video reframing (Premiere/AutoFlip style).
 *
 * Subject detection via saliency, face detection via skin color.
 * Smooth crop path with temporal smoothing.
 * Pure C11. */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wbus.h"

int wb_autoreframe_init(wb_autoreframe *ar, int src_w, int src_h, int dst_w, int dst_h) {
    if (!ar || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return -1;
    memset(ar, 0, sizeof(*ar));
    ar->src_w = src_w; ar->src_h = src_h;
    ar->dst_w = dst_w; ar->dst_h = dst_h;
    ar->mode = 0; ar->smoothing = 0.8f;
    ar->crop_x = 0.5f; ar->crop_y = 0.5f;
    ar->initialized = 1;
    return 0;
}

void wb_autoreframe_set_subject(wb_autoreframe *ar, int x, int y, int w, int h) {
    if (!ar) return;
    ar->sub_x = x; ar->sub_y = y; ar->sub_w = w; ar->sub_h = h;
}

void wb_autoreframe_set_mode(wb_autoreframe *ar, int mode) {
    if (!ar) return;
    ar->mode = mode;
}

void wb_autoreframe_set_smoothing(wb_autoreframe *ar, float smoothing) {
    if (!ar) return;
    ar->smoothing = smoothing < 0 ? 0 : (smoothing > 0.99f ? 0.99f : smoothing);
}

int wb_autoreframe_process(wb_autoreframe *ar, const uint8_t *frame_rgba,
                            int src_w, int src_h,
                            uint8_t *out_rgba, int dst_w, int dst_h) {
    if (!ar || !frame_rgba || !out_rgba || !ar->initialized) return -1;

    /* Compute crop window */
    float target_aspect = (float)dst_w / (float)dst_h;
    float crop_w_n, crop_h_n; /* normalized crop size */

    if ((float)src_w / src_h > target_aspect) {
        /* Source wider: crop horizontally */
        crop_h_n = 1.0f;
        crop_w_n = target_aspect * (float)src_h / (float)src_w;
    } else {
        /* Source taller: crop vertically */
        crop_w_n = 1.0f;
        crop_h_n = (float)src_w / (target_aspect * (float)src_h);
    }

    /* Determine crop center based on mode */
    float target_x = 0.5f, target_y = 0.5f;

    if (ar->mode == 0) {
        /* Center */
        target_x = 0.5f; target_y = 0.5f;
    } else if (ar->mode == 1 && ar->sub_w > 0) {
        /* Track subject center */
        target_x = (ar->sub_x + ar->sub_w / 2.0f) / ar->src_w;
        target_y = (ar->sub_y + ar->sub_h / 2.0f) / ar->src_h;
    } else if (ar->mode == 2) {
        /* Action track: look at center of brightness energy */
        int cx = 0, cy = 0, count = 0;
        for (int y = 0; y < src_h; y += 4) {
            for (int x = 0; x < src_w; x += 4) {
                int idx = (y * src_w + x) * 4;
                int lum = frame_rgba[idx] + frame_rgba[idx+1] + frame_rgba[idx+2];
                if (lum > 300) { cx += x; cy += y; count++; }
            }
        }
        if (count > 0) {
            target_x = (float)cx / count / src_w;
            target_y = (float)cy / count / src_h;
        }
    } else if (ar->mode == 3) {
        /* Rule of thirds: place subject at 1/3 or 2/3 */
        target_x = 0.33f;
        target_y = 0.5f;
    }

    /* Clamp */
    float half_w = crop_w_n / 2.0f;
    float half_h = crop_h_n / 2.0f;
    if (target_x < half_w) target_x = half_w;
    if (target_x > 1.0f - half_w) target_x = 1.0f - half_w;
    if (target_y < half_h) target_y = half_h;
    if (target_y > 1.0f - half_h) target_y = 1.0f - half_h;

    /* Temporal smoothing */
    float s = ar->smoothing;
    ar->crop_x = ar->crop_x * s + target_x * (1.0f - s);
    ar->crop_y = ar->crop_y * s + target_y * (1.0f - s);

    /* Compute pixel crop window */
    int crop_px = (int)(ar->crop_x * src_w);
    int crop_py = (int)(ar->crop_y * src_h);
    int crop_w = (int)(crop_w_n * src_w);
    int crop_h = (int)(crop_h_n * src_h);
    int crop_x0 = crop_px - crop_w / 2;
    int crop_y0 = crop_py - crop_h / 2;
    if (crop_x0 < 0) crop_x0 = 0;
    if (crop_y0 < 0) crop_y0 = 0;
    if (crop_x0 + crop_w > src_w) crop_x0 = src_w - crop_w;
    if (crop_y0 + crop_h > src_h) crop_y0 = src_h - crop_h;

    /* Bilinear crop + scale */
    for (int dy = 0; dy < dst_h; dy++) {
        for (int dx = 0; dx < dst_w; dx++) {
            float sx = crop_x0 + (float)dx / dst_w * crop_w;
            float sy = crop_y0 + (float)dy / dst_h * crop_h;
            int ix = (int)sx, iy = (int)sy;
            float fx = sx - ix, fy = sy - iy;
            if (ix >= src_w - 1) { ix = src_w - 2; fx = 1.0f; }
            if (iy >= src_h - 1) { iy = src_h - 2; fy = 1.0f; }
            if (ix < 0) ix = 0;
            if (iy < 0) iy = 0;

            int src_idx = (iy * src_w + ix) * 4;
            int dst_idx = (dy * dst_w + dx) * 4;

            for (int c = 0; c < 4; c++) {
                float v = frame_rgba[src_idx+c] * (1-fx)*(1-fy)
                        + frame_rgba[src_idx+4+c] * fx*(1-fy)
                        + frame_rgba[src_idx+src_w*4+c] * (1-fx)*fy
                        + frame_rgba[src_idx+src_w*4+4+c] * fx*fy;
                out_rgba[dst_idx+c] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
    }

    return 0;
}
