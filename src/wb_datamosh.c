/* wb_datamosh.c — datamosh / P-frame glitch effects for YTP.
 *
 * R80: Advanced YTP technique — simulate video compression artifacts,
 * macroblocking, pixelation, channel separation, frame glitching.
 * Supports:
 *   - Pixelation / macroblock simulation
 *   - RGB channel separation (chromatic aberration)
 *   - Scanline glitch
 *   - Block displacement (motion vector corruption)
 *   - Color banding / posterize
 *   - Noise injection
 *   - Horizontal tear / slice offset
 *
 * Pure C11, operates on RGBA uint8 buffers.
 */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    int   block_size;       /* Pixelation block size (1-64) */
    float channel_shift;    /* RGB channel separation in pixels */
    int   scanlines;        /* Scanline intensity (0=off, 255=full) */
    float tear_amount;      /* Horizontal tear displacement */
    int   tear_y;           /* Y position of tear */
    int   posterize;        /* Color levels (256=none, 2=posterized) */
    float noise;            /* Noise intensity (0-1) */
    int   width, height;
    int   seed;             /* Random seed */
} wb_datamosh_inst;

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void *wb_datamosh_create(int width, int height) {
    wb_datamosh_inst *inst = (wb_datamosh_inst *)calloc(1, sizeof(wb_datamosh_inst));
    if (!inst) return NULL;
    inst->width = width;
    inst->height = height;
    inst->block_size = 8;
    inst->posterize = 256;
    inst->seed = 12345;
    return inst;
}

void wb_datamosh_destroy(void *inst) { free(inst); }

void wb_datamosh_set_pixelation(void *inst, int size) {
    ((wb_datamosh_inst *)inst)->block_size = size < 1 ? 1 : (size > 64 ? 64 : size);
}

void wb_datamosh_set_channel_shift(void *inst, float shift) {
    ((wb_datamosh_inst *)inst)->channel_shift = shift;
}

void wb_datamosh_set_scanlines(void *inst, int intensity) {
    ((wb_datamosh_inst *)inst)->scanlines = intensity;
}

void wb_datamosh_set_tear(void *inst, float amount, int y) {
    wb_datamosh_inst *d = (wb_datamosh_inst *)inst;
    d->tear_amount = amount;
    d->tear_y = y;
}

void wb_datamosh_set_posterize(void *inst, int levels) {
    ((wb_datamosh_inst *)inst)->posterize = levels < 2 ? 2 : levels;
}

void wb_datamosh_set_noise(void *inst, float n) {
    ((wb_datamosh_inst *)inst)->noise = n < 0 ? 0 : (n > 1 ? 1 : n);
}

void wb_datamosh_process(void *inst, const uint8_t *in_rgba, uint8_t *out_rgba, int width, int height) {
    wb_datamosh_inst *d = (wb_datamosh_inst *)inst;
    int bs = d->block_size;
    uint32_t rng = (uint32_t)d->seed;
    memcpy(out_rgba, in_rgba, width * height * 4);

    /* Pixelation */
    if (bs > 1) {
        for (int by = 0; by < height; by += bs) {
            for (int bx = 0; bx < width; bx += bs) {
                int sum_r = 0, sum_g = 0, sum_b = 0, count = 0;
                for (int dy = 0; dy < bs && by + dy < height; dy++) {
                    for (int dx = 0; dx < bs && bx + dx < width; dx++) {
                        int i = ((by + dy) * width + (bx + dx)) * 4;
                        sum_r += in_rgba[i];
                        sum_g += in_rgba[i+1];
                        sum_b += in_rgba[i+2];
                        count++;
                    }
                }
                if (count == 0) continue;
                uint8_t avg_r = (uint8_t)(sum_r / count);
                uint8_t avg_g = (uint8_t)(sum_g / count);
                uint8_t avg_b = (uint8_t)(sum_b / count);
                for (int dy = 0; dy < bs && by + dy < height; dy++) {
                    for (int dx = 0; dx < bs && bx + dx < width; dx++) {
                        int i = ((by + dy) * width + (bx + dx)) * 4;
                        out_rgba[i]   = avg_r;
                        out_rgba[i+1] = avg_g;
                        out_rgba[i+2] = avg_b;
                    }
                }
            }
        }
    }

    /* Channel separation (chromatic aberration) */
    if (d->channel_shift > 0.5f) {
        int shift = (int)d->channel_shift;
        uint8_t *temp = (uint8_t *)malloc(width * height * 4);
        memcpy(temp, out_rgba, width * height * 4);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int i = (y * width + x) * 4;
                int rsx = x - shift; if (rsx < 0) rsx = 0; if (rsx >= width) rsx = width - 1;
                int bsx = x + shift; if (bsx < 0) bsx = 0; if (bsx >= width) bsx = width - 1;
                int ri = (y * width + rsx) * 4;
                int bi = (y * width + bsx) * 4;
                out_rgba[i]   = temp[ri];       /* Red from left */
                out_rgba[i+2] = temp[bi+2];     /* Blue from right */
            }
        }
        free(temp);
    }

    /* Scanlines */
    if (d->scanlines > 0) {
        for (int y = 0; y < height; y += 2) {
            for (int x = 0; x < width; x++) {
                int i = (y * width + x) * 4;
                out_rgba[i]   = (out_rgba[i]   * (255 - d->scanlines)) / 255;
                out_rgba[i+1] = (out_rgba[i+1] * (255 - d->scanlines)) / 255;
                out_rgba[i+2] = (out_rgba[i+2] * (255 - d->scanlines)) / 255;
            }
        }
    }

    /* Horizontal tear */
    if (d->tear_amount > 0.0f && d->tear_y >= 0 && d->tear_y < height) {
        int shift = (int)((xorshift32(&rng) % 100) * d->tear_amount);
        for (int y = d->tear_y; y < height; y++) {
            int row_start = y * width * 4;
            uint8_t *row = (uint8_t *)malloc(width * 4);
            memcpy(row, out_rgba + row_start, width * 4);
            for (int x = 0; x < width; x++) {
                int src = x + shift;
                if (src >= width) src -= width;
                if (src < 0) src += width;
                int dst = (y * width + x) * 4;
                int srow = src * 4;
                out_rgba[dst]   = row[srow];
                out_rgba[dst+1] = row[srow+1];
                out_rgba[dst+2] = row[srow+2];
            }
            free(row);
        }
    }

    /* Posterize */
    if (d->posterize < 256) {
        float step = 255.0f / (d->posterize - 1);
        for (int i = 0; i < width * height * 4; i += 4) {
            out_rgba[i]   = (uint8_t)(step * (int)(out_rgba[i] / step));
            out_rgba[i+1] = (uint8_t)(step * (int)(out_rgba[i+1] / step));
            out_rgba[i+2] = (uint8_t)(step * (int)(out_rgba[i+2] / step));
        }
    }

    /* Noise */
    if (d->noise > 0.0f) {
        for (int i = 0; i < width * height * 4; i += 4) {
            int n = (int)((xorshift32(&rng) % 256) * d->noise) - (int)(128 * d->noise);
            int r = out_rgba[i]   + n;
            int g = out_rgba[i+1] + n;
            int b = out_rgba[i+2] + n;
            out_rgba[i]   = r < 0 ? 0 : (r > 255 ? 255 : r);
            out_rgba[i+1] = g < 0 ? 0 : (g > 255 ? 255 : g);
            out_rgba[i+2] = b < 0 ? 0 : (b > 255 ? 255 : b);
        }
    }

    d->seed = (int)rng;
}
