/* wb_reaction.c — reaction video format (side-by-side split screen).
 *
 * R077: Combine two videos side-by-side for reaction content.
 *
 * Layouts:
 *   0: Side by side (50/50)
 *   1: Picture-in-picture (main + small overlay)
 *   2: Top/bottom (50/50)
 *   3: Main + small corner (reaction cam)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef enum {
    REACTION_SIDE_BY_SIDE = 0,
    REACTION_PIP,
    REACTION_TOP_BOTTOM,
    REACTION_CORNER
} reaction_layout_t;

typedef struct {
    reaction_layout_t layout;
    float    pip_scale;      /* PIP window scale (0.2..0.5) */
    float    border_px;      /* Border width in pixels */
    uint8_t  border_r, border_g, border_b;
} wb_reaction_inst;

void *wb_reaction_create(void) {
    wb_reaction_inst *r = (wb_reaction_inst *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->layout = REACTION_SIDE_BY_SIDE;
    r->pip_scale = 0.3f;
    r->border_px = 2.0f;
    r->border_r = 255;
    r->border_g = 255;
    r->border_b = 255;
    return r;
}

void wb_reaction_destroy(void *inst) { free(inst); }

void wb_reaction_set(void *inst, int param, float v) {
    wb_reaction_inst *r = (wb_reaction_inst *)inst;
    if (!r) return;
    switch (param) {
    case 0: r->layout = (reaction_layout_t)(int)v; break;
    case 1: r->pip_scale = v < 0.1f ? 0.1f : (v > 0.5f ? 0.5f : v); break;
    default: break;
    }
}

/* Blend two RGBA frames into one output frame.
 * main: primary video frame (main_w × main_h)
 * overlay: secondary video frame (overlay_w × overlay_h)
 * out: output frame (out_w × out_h)
 * All frames are RGBA (4 bytes per pixel) */
void wb_reaction_composite(const wb_reaction_inst *r,
                            const uint8_t *main, int main_w, int main_h,
                            const uint8_t *overlay, int overlay_w, int overlay_h,
                            uint8_t *out, int out_w, int out_h) {
    if (!r || !main || !overlay || !out) return;

    /* Clear output to black */
    memset(out, 0, out_w * out_h * 4);

    switch (r->layout) {
    case REACTION_SIDE_BY_SIDE: {
        /* Main on left half, overlay on right half */
        int half_w = out_w / 2;
        for (int y = 0; y < out_h; y++) {
            for (int x = 0; x < half_w; x++) {
                /* Scale main to fit left half */
                int src_x = x * main_w / half_w;
                int src_y = y * main_h / out_h;
                if (src_x >= main_w) src_x = main_w - 1;
                if (src_y >= main_h) src_y = main_h - 1;
                int dst_idx = (y * out_w + x) * 4;
                int src_idx = (src_y * main_w + src_x) * 4;
                out[dst_idx] = main[src_idx];
                out[dst_idx+1] = main[src_idx+1];
                out[dst_idx+2] = main[src_idx+2];
                out[dst_idx+3] = 255;
            }
            for (int x = half_w; x < out_w; x++) {
                /* Scale overlay to fit right half */
                int src_x = (x - half_w) * overlay_w / (out_w - half_w);
                int src_y = y * overlay_h / out_h;
                if (src_x >= overlay_w) src_x = overlay_w - 1;
                if (src_y >= overlay_h) src_y = overlay_h - 1;
                int dst_idx = (y * out_w + x) * 4;
                int src_idx = (src_y * overlay_w + src_x) * 4;
                out[dst_idx] = overlay[src_idx];
                out[dst_idx+1] = overlay[src_idx+1];
                out[dst_idx+2] = overlay[src_idx+2];
                out[dst_idx+3] = 255;
            }
        }
        break;
    }

    case REACTION_PIP: {
        /* Main fills output, overlay in corner */
        /* First, scale main to fill output */
        for (int y = 0; y < out_h; y++) {
            for (int x = 0; x < out_w; x++) {
                int src_x = x * main_w / out_w;
                int src_y = y * main_h / out_h;
                int dst_idx = (y * out_w + x) * 4;
                int src_idx = (src_y * main_w + src_x) * 4;
                out[dst_idx] = main[src_idx];
                out[dst_idx+1] = main[src_idx+1];
                out[dst_idx+2] = main[src_idx+2];
                out[dst_idx+3] = 255;
            }
        }
        /* Overlay in bottom-right corner */
        int pip_w = (int)(out_w * r->pip_scale);
        int pip_h = (int)(pip_w * (float)overlay_h / (float)overlay_w);
        int pip_x = out_w - pip_w - 10;
        int pip_y = out_h - pip_h - 10;

        for (int y = 0; y < pip_h; y++) {
            for (int x = 0; x < pip_w; x++) {
                int src_x = x * overlay_w / pip_w;
                int src_y = y * overlay_h / pip_h;
                int dst_x = pip_x + x;
                int dst_y = pip_y + y;
                if (dst_x >= 0 && dst_x < out_w && dst_y >= 0 && dst_y < out_h) {
                    int dst_idx = (dst_y * out_w + dst_x) * 4;
                    int src_idx = (src_y * overlay_w + src_x) * 4;
                    out[dst_idx] = overlay[src_idx];
                    out[dst_idx+1] = overlay[src_idx+1];
                    out[dst_idx+2] = overlay[src_idx+2];
                    out[dst_idx+3] = 255;
                }
            }
        }
        break;
    }

    case REACTION_TOP_BOTTOM: {
        int half_h = out_h / 2;
        for (int y = 0; y < half_h; y++) {
            for (int x = 0; x < out_w; x++) {
                int src_x = x * main_w / out_w;
                int src_y = y * main_h / half_h;
                int dst_idx = (y * out_w + x) * 4;
                int src_idx = (src_y * main_w + src_x) * 4;
                out[dst_idx] = main[src_idx];
                out[dst_idx+1] = main[src_idx+1];
                out[dst_idx+2] = main[src_idx+2];
                out[dst_idx+3] = 255;
            }
        }
        for (int y = half_h; y < out_h; y++) {
            for (int x = 0; x < out_w; x++) {
                int src_x = x * overlay_w / out_w;
                int src_y = (y - half_h) * overlay_h / (out_h - half_h);
                int dst_idx = (y * out_w + x) * 4;
                int src_idx = (src_y * overlay_w + src_x) * 4;
                out[dst_idx] = overlay[src_idx];
                out[dst_idx+1] = overlay[src_idx+1];
                out[dst_idx+2] = overlay[src_idx+2];
                out[dst_idx+3] = 255;
            }
        }
        break;
    }

    default:
        break;
    }
}
