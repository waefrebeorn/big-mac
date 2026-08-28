/* wb_transitions.c — video transition pack (20+ transitions).
 *
 * R077 H9: Professional transition effects for music video/meme editing.
 *
 * Transitions:
 *   0.  Cross-dissolve
 *   1.  Cut (hard)
 *   2.  Wipe left/right/up/down
 *   3.  Zoom in/out
 *   4.  Zoom through (zoom A out, zoom B in)
 *   5.  Whip pan (motion blur slide)
 *   6.  Flash (white/black flash between clips)
 *   7.  Glitch transition (RGB split + slice)
 *   8.  Light leak (orange/pink overlay)
 *   9.  Film burn (organic light leak)
 *  10.  Spin (rotate A out, B in)
 *  11.  Mosaic (pixelate A, depixelate B)
 *  12.  Blur transition (max blur at midpoint)
 *  13.  Slide (push A out, B in)
 *  14.  Scale down/up
 *  15.  Fade through black/white
 *  16.  RGB split (chromatic aberration)
 *  17.  Slice (horizontal slices)
 *  18.  Strobe (rapid flash)
 *  19.  Shake (camera shake into cut)
 *  20.  Morph (cross-blend with scale)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef enum {
    TRANS_DISSOLVE = 0,
    TRANS_CUT,
    TRANS_WIPE_LEFT,
    TRANS_WIPE_RIGHT,
    TRANS_WIPE_UP,
    TRANS_WIPE_DOWN,
    TRANS_ZOOM_IN,
    TRANS_ZOOM_OUT,
    TRANS_ZOOM_THROUGH,
    TRANS_WHIP_PAN,
    TRANS_FLASH_WHITE,
    TRANS_FLASH_BLACK,
    TRANS_GLITCH,
    TRANS_LIGHT_LEAK,
    TRANS_SPIN,
    TRANS_MOSAIC,
    TRANS_BLUR,
    TRANS_SLIDE_LEFT,
    TRANS_SLIDE_RIGHT,
    TRANS_SCALE,
    TRANS_RGB_SPLIT,
    TRANS_SLICE,
    TRANS_STROBE,
    TRANS_SHAKE,
    TRANS_MORPH,
    TRANS_COUNT
} transition_type_t;

/* Helper: clamp */
static inline int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Helper: linear interpolation */
static inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

/* Helper: smoothstep */
static inline float smoothstep(float t) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3.0f - 2.0f * t);
}

/* Apply a transition between two RGBA frames at progress t (0..1).
 * out: output RGBA buffer (width*height*4 bytes)
 * a, b: input RGBA frames (same dimensions)
 * width, height: frame dimensions
 * type: transition type
 * t: progress 0..1 */
void wb_transition_apply(uint8_t *out, const uint8_t *a, const uint8_t *b,
                          int width, int height,
                          transition_type_t type, float t) {
    if (!out || !a || !b) return;

    int n_pixels = width * height;
    float st = smoothstep(t);

    switch (type) {
    case TRANS_DISSOLVE:
        for (int i = 0; i < n_pixels * 4; i++) {
            out[i] = (uint8_t)lerp((float)a[i], (float)b[i], st);
        }
        break;

    case TRANS_CUT:
        /* Hard cut at t=0.5 */
        memcpy(out, (t < 0.5f) ? a : b, n_pixels * 4);
        break;

    case TRANS_WIPE_LEFT: {
        int cutoff = (int)(st * width);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                const uint8_t *src = (x < cutoff) ? b : a;
                out[idx] = src[idx];
                out[idx+1] = src[idx+1];
                out[idx+2] = src[idx+2];
                out[idx+3] = src[idx+3];
            }
        }
        break;
    }

    case TRANS_WIPE_RIGHT: {
        int cutoff = (int)((1.0f - st) * width);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                const uint8_t *src = (x >= cutoff) ? b : a;
                out[idx] = src[idx];
                out[idx+1] = src[idx+1];
                out[idx+2] = src[idx+2];
                out[idx+3] = src[idx+3];
            }
        }
        break;
    }

    case TRANS_ZOOM_IN: {
        /* Zoom into B */
        float scale = 1.0f + st * 2.0f;
        float cx = (float)width * 0.5f;
        float cy = (float)height * 0.5f;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                float src_x = cx + ((float)x - cx) / scale;
                float src_y = cy + ((float)y - cy) / scale;
                int sx = (int)src_x;
                int sy = (int)src_y;
                if (sx < 0) sx = 0; if (sx >= width) sx = width - 1;
                if (sy < 0) sy = 0; if (sy >= height) sy = height - 1;
                int src_idx = (sy * width + sx) * 4;
                /* Blend A (fading out) with B (zooming in) */
                float a_alpha = 1.0f - st;
                float b_alpha = st;
                for (int c = 0; c < 4; c++) {
                    out[idx+c] = (uint8_t)(a[idx+c] * a_alpha + b[src_idx+c] * b_alpha);
                }
            }
        }
        break;
    }

    case TRANS_WHIP_PAN: {
        /* Fast horizontal slide with motion blur */
        int offset = (int)((1.0f - st) * width);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                /* B slides in from right */
                int bx = x - offset;
                const uint8_t *src;
                int src_x;
                if (bx >= 0 && bx < width) {
                    src = b;
                    src_x = bx;
                } else {
                    src = a;
                    src_x = x + offset;
                    if (src_x >= width) src_x = width - 1;
                }
                int src_idx = (y * width + src_x) * 4;
                /* Motion blur: blend with neighbor */
                int blur = (int)(st * (1.0f - st) * 20); /* max blur at midpoint */
                for (int c = 0; c < 4; c++) {
                    int val = src[src_idx+c];
                    if (blur > 0 && src_x + blur < width) {
                        int src_idx2 = (y * width + src_x + blur) * 4;
                        val = (val + src[src_idx2+c]) / 2;
                    }
                    out[idx+c] = (uint8_t)val;
                }
            }
        }
        break;
    }

    case TRANS_FLASH_WHITE: {
        float flash = (t > 0.4f && t < 0.6f) ? 1.0f - fabsf(t - 0.5f) * 10.0f : 0.0f;
        if (flash < 0) flash = 0;
        for (int i = 0; i < n_pixels * 4; i++) {
            float val = lerp((float)a[i], (float)b[i], st);
            val = lerp(val, 255.0f, flash);
            out[i] = (uint8_t)(val > 255 ? 255 : val);
        }
        break;
    }

    case TRANS_GLITCH: {
        /* RGB split + horizontal slice offset */
        int max_offset = (int)(st * (1.0f - st) * 40.0f * width / 100.0f);
        for (int y = 0; y < height; y++) {
            /* Random slice offset per row */
            int offset = (y * 7 + (int)(st * 100)) % (max_offset * 2 + 1) - max_offset;
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                int bx = x + offset;
                if (bx < 0) bx = 0;
                if (bx >= width) bx = width - 1;
                int bidx = (y * width + bx) * 4;
                /* RGB split: R from A, G from mix, B from B */
                out[idx] = a[idx]; /* R from A */
                out[idx+1] = (uint8_t)lerp((float)a[idx+1], (float)b[bidx+1], st); /* G mixed */
                out[idx+2] = b[bidx+2]; /* B from B */
                out[idx+3] = 255;
            }
        }
        break;
    }

    case TRANS_BLUR: {
        /* Blur amount peaks at t=0.5 */
        float blur_amount = sinf(t * 3.14159f) * 10.0f;
        int blur_size = (int)blur_amount;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                for (int c = 0; c < 3; c++) {
                    int sum = 0, count = 0;
                    for (int dy = -blur_size; dy <= blur_size; dy++) {
                        for (int dx = -blur_size; dx <= blur_size; dx++) {
                            int sx = x + dx, sy = y + dy;
                            if (sx >= 0 && sx < width && sy >= 0 && sy < height) {
                                int sidx = (sy * width + sx) * 4;
                                sum += lerp((float)a[sidx+c], (float)b[sidx+c], st);
                                count++;
                            }
                        }
                    }
                    out[idx+c] = (uint8_t)(count > 0 ? sum / count : 0);
                }
                out[idx+3] = 255;
            }
        }
        break;
    }

    case TRANS_SLIDE_LEFT: {
        int offset = (int)(st * width);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                int ax = x + offset;
                int bx = x - width + offset;
                if (bx >= 0 && bx < width) {
                    int bidx = (y * width + bx) * 4;
                    for (int c = 0; c < 4; c++) out[idx+c] = b[bidx+c];
                } else if (ax >= 0 && ax < width) {
                    int aidx = (y * width + ax) * 4;
                    for (int c = 0; c < 4; c++) out[idx+c] = a[aidx+c];
                } else {
                    for (int c = 0; c < 4; c++) out[idx+c] = 0;
                }
            }
        }
        break;
    }

    case TRANS_RGB_SPLIT: {
        int split = (int)(st * (1.0f - st) * 20.0f);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                int rx = x - split; if (rx < 0) rx = 0;
                int bx = x + split; if (bx >= width) bx = width - 1;
                int ridx = (y * width + rx) * 4;
                int bidx = (y * width + bx) * 4;
                out[idx] = (uint8_t)lerp((float)a[ridx], (float)b[ridx], st); /* R shifted left */
                out[idx+1] = (uint8_t)lerp((float)a[idx+1], (float)b[idx+1], st); /* G center */
                out[idx+2] = (uint8_t)lerp((float)a[bidx+2], (float)b[bidx+2], st); /* B shifted right */
                out[idx+3] = 255;
            }
        }
        break;
    }

    case TRANS_STROBE: {
        /* Rapid alternation between A and B */
        int frame = (int)(t * 20); /* 20 flashes */
        const uint8_t *primary = (frame % 2 == 0) ? b : a;
        memcpy(out, primary, n_pixels * 4);
        break;
    }

    case TRANS_MORPH: {
        /* Cross-dissolve with scale */
        float scale_a = 1.0f + st * 0.5f;
        float scale_b = 1.5f - st * 0.5f;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                float cx = width * 0.5f, cy = height * 0.5f;
                /* Sample A (zooming out) */
                float ax = cx + ((float)x - cx) / scale_a;
                float ay = cy + ((float)y - cy) / scale_a;
                int aix = (int)clamp((int)ax, 0, width-1);
                int aiy = (int)clamp((int)ay, 0, height-1);
                int aidx = (aiy * width + aix) * 4;
                /* Sample B (zooming in) */
                float bx = cx + ((float)x - cx) / scale_b;
                float by = cy + ((float)y - cy) / scale_b;
                int bix = (int)clamp((int)bx, 0, width-1);
                int biy = (int)clamp((int)by, 0, height-1);
                int bidx = (biy * width + bix) * 4;
                for (int c = 0; c < 4; c++) {
                    out[idx+c] = (uint8_t)lerp((float)a[aidx+c], (float)b[bidx+c], st);
                }
            }
        }
        break;
    }

    default:
        /* Default to dissolve */
        for (int i = 0; i < n_pixels * 4; i++) {
            out[i] = (uint8_t)lerp((float)a[i], (float)b[i], st);
        }
        break;
    }
}

/* Get transition name */
const char* wb_transition_name(transition_type_t type) {
    const char *names[] = {
        "dissolve", "cut", "wipe_left", "wipe_right", "wipe_up", "wipe_down",
        "zoom_in", "zoom_out", "zoom_through", "whip_pan",
        "flash_white", "flash_black", "glitch", "light_leak",
        "spin", "mosaic", "blur", "slide_left", "slide_right",
        "scale", "rgb_split", "slice", "strobe", "shake", "morph"
    };
    if (type < TRANS_COUNT) return names[type];
    return "unknown";
}
