/* wb_tga.c — R074 hop 181 (G-SF014): minimal TGA loader.
 * Supports uncompressed type-2 true-color 24/32-bit, origin top-left or
 * bottom-left. Everything else is rejected loudly. Pure C11. */
#include "wbus/wbus_tga.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int wb_tga_load(wb_tga *t, const char *path) {
    if (!t || !path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[18];
    if (fread(hdr, 1, 18, f) != 18) { fclose(f); return -1; }
    int id_len   = hdr[0];
    int cmap     = hdr[1];
    int type     = hdr[2];
    int w        = hdr[12] | (hdr[13] << 8);
    int h        = hdr[14] | (hdr[15] << 8);
    int bpp      = hdr[16];
    int desc     = hdr[17];
    if (cmap != 0 || type != 2 || (bpp != 24 && bpp != 32) ||
        w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        fclose(f);
        return -1;
    }
    /* skip image id + color map data (none for cmap=0, but be safe) */
    fseek(f, id_len, SEEK_CUR);
    t->w = w; t->h = h;
    t->px = malloc((size_t)w * h * 4);
    if (!t->px) { fclose(f); return -1; }
    int bytes = bpp / 8;
    uint8_t *row = malloc((size_t)w * bytes);
    if (!row) { free(t->px); fclose(f); return -1; }
    int top_origin = (desc & 0x20) != 0;
    for (int y = 0; y < h; y++) {
        if (fread(row, 1, (size_t)w * bytes, f) != (size_t)w * bytes) {
            free(row); free(t->px); t->px = NULL;
            fclose(f); return -1;
        }
        int dy = top_origin ? y : (h - 1 - y);
        uint8_t *dst = t->px + (size_t)dy * w * 4;
        for (int x = 0; x < w; x++) {
            const uint8_t *s = row + x * bytes;
            dst[x*4+0] = s[2];   /* TGA stores BGR(A) */
            dst[x*4+1] = s[1];
            dst[x*4+2] = s[0];
            dst[x*4+3] = bytes == 4 ? s[3] : 255;
        }
    }
    free(row);
    fclose(f);
    return 0;
}

void wb_tga_free(wb_tga *t) {
    if (!t) return;
    free(t->px);
    t->px = NULL;
}
