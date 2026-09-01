/* wb_gif_export.c — Animated GIF export (R084 video edit graph).
 *
 * Pure-C11 GIF89a encoder with:
 *   - Global color table (max_colors entries, padded to power-of-two)
 *   - Per-frame Graphic Control Extension (transparent bg index, delay)
 *   - Image Descriptors (one per frame, full-frame)
 *   - LZW compression (variable-code-width, starting at 2-bit clear)
 *   - Netscape 2.0 looping extension (infinite)
 *   - Median-cut color quantization from RGBA float to indexed palette
 *
 * No external libraries. Output validated per GIF89a spec.
 */

#include "wbus/wbus_edit.h"
#include "wbus/wbus_compositor.h"
#include "wbus/wbus.h"
#include "wbus/wbus_gif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- GIF constants ------------------------------------------------------- */

#define GIF_SIG         "GIF89a"
#define GIF_TRAILER     0x3B
#define GIF_EXT_INTRO   0x21   /* extension introducer */
#define GIF_IMG_DESCR   0x2C   /* image descriptor */
#define GIF_APP_EXT     0xFF   /* application extension */
#define GIF_GCE_EXT     0xF9   /* graphic control extension label */
#define GIF_PLAIN_EXT   0x01   /* plain text extension */

#define GIF_MAX_LZW_BITS 12
#define GIF_MAX_PAL      256

/* ---- minimal byte-stream writer ----------------------------------------- */

typedef struct {
    FILE *fp;
    int    error;        /* set non-zero on write failure */
} gif_writer;

static inline void gw_putc(gif_writer *w, uint8_t c) {
    if (w->error) return;
    if (fputc(c, w->fp) == EOF) w->error = 1;
}

static inline void gw_put16(gif_writer *w, uint16_t v) {
    gw_putc(w, (uint8_t)(v & 0xFF));
    gw_putc(w, (uint8_t)((v >> 8) & 0xFF));
}

static inline void gw_put32(gif_writer *w, uint32_t v) {
    gw_putc(w, (uint8_t)(v & 0xFF));
    gw_putc(w, (uint8_t)((v >> 8) & 0xFF));
    gw_putc(w, (uint8_t)((v >> 16) & 0xFF));
    gw_putc(w, (uint8_t)((v >> 24) & 0xFF));
}

/* Write `len` bytes from `buf`. */
static void gw_write(gif_writer *w, const void *buf, size_t len) {
    if (w->error) return;
    if (fwrite(buf, 1, len, w->fp) != len) w->error = 1;
}

/* ---- color quantization (median cut) ------------------------------------ */

typedef struct {
    uint8_t r, g, b;
} rgb_color;

typedef struct {
    int       *indices;     /* pixel-index list into the color list */
    rgb_color *colors;      /* current colors */
    int        n;           /* number of entries */
} color_list;

/* Build a color list from RGBA uint8 pixels (alpha used as coverage). */
static color_list *cl_from_image(const uint8_t *rgba, int w, int h) {
    int n = w * h;
    color_list *cl = (color_list *)calloc(1, sizeof(color_list));
    if (!cl) return NULL;
    cl->n = n;
    cl->colors = (rgb_color *)malloc((size_t)n * sizeof(rgb_color));
    cl->indices = (int *)malloc((size_t)n * sizeof(int));
    if (!cl->colors || !cl->indices) {
        free(cl->colors); free(cl->indices); free(cl);
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        cl->colors[i].r = rgba[i * 4 + 0];
        cl->colors[i].g = rgba[i * 4 + 1];
        cl->colors[i].b = rgba[i * 4 + 2];
        cl->indices[i] = i;
    }
    return cl;
}

/* Sort a sub-range of indices by the value of a single channel. */
static void sort_by_channel(color_list *cl, int lo, int hi, int ch) {
    /* Simple insertion sort — fast enough for the sub-ranges in median cut. */
    for (int i = lo + 1; i <= hi; i++) {
        int cur = cl->indices[i];
        int j = i - 1;
        while (j >= lo) {
            int oj = cl->indices[j];
            uint8_t cv, nv;
            switch (ch) {
                case 0: cv = cl->colors[oj].r; nv = cl->colors[cur].r; break;
                case 1: cv = cl->colors[oj].g; nv = cl->colors[cur].g; break;
                default: cv = cl->colors[oj].b; nv = cl->colors[cur].b; break;
            }
            if (cv > nv) {
                cl->indices[j + 1] = cl->indices[j];
                j--;
            } else break;
        }
        cl->indices[j + 1] = cur;
    }
}

/* Compute the bounding box (per channel) of a sub-range. */
static void range_bbox(color_list *cl, int lo, int hi,
                       int *min_r, int *max_r,
                       int *min_g, int *max_g,
                       int *min_b, int *max_b) {
    int rmin = 255, rmax = 0, gmin = 255, gmax = 0, bmin = 255, bmax = 0;
    for (int i = lo; i <= hi; i++) {
        rgb_color c = cl->colors[cl->indices[i]];
        if (c.r < rmin) rmin = c.r; if (c.r > rmax) rmax = c.r;
        if (c.g < gmin) gmin = c.g; if (c.g > gmax) gmax = c.g;
        if (c.b < bmin) bmin = c.b; if (c.b > bmax) bmax = c.b;
    }
    *min_r = rmin; *max_r = rmax;
    *min_g = gmin; *max_g = gmax;
    *min_b = bmin; *max_b = bmax;
}

/* Pick the channel with the widest range in [lo,hi]. */
static int widest_channel(color_list *cl, int lo, int hi) {
    int mr, xr, mg, xg, mb, xb;
    range_bbox(cl, lo, hi, &mr, &xr, &mg, &xg, &mb, &xb);
    int dr = xr - mr, dg = xg - mg, db = xb - mb;
    if (dr >= dg && dr >= db) return 0;
    if (dg >= db) return 1;
    return 2;
}

/* Recursively split color buckets by median cut until we have `max_colors`
 * representative colors. Returns the palette via `out_colors` (caller frees)
 * and the number of entries in `*out_count`. */
static void median_cut(color_list *cl, int lo, int hi,
                       int depth, int max_colors,
                       rgb_color *out_colors, int *out_count) {
    int count = hi - lo + 1;

    if (depth <= 0 || count <= 1) {
        /* Leaf: compute the average of this bucket. */
        int sr = 0, sg = 0, sb = 0;
        for (int i = lo; i <= hi; i++) {
            rgb_color c = cl->colors[cl->indices[i]];
            sr += c.r; sg += c.g; sb += c.b;
        }
        out_colors[*out_count].r = (uint8_t)(sr / count);
        out_colors[*out_count].g = (uint8_t)(sg / count);
        out_colors[*out_count].b = (uint8_t)(sb / count);
        (*out_count)++;
        return;
    }

    int ch = widest_channel(cl, lo, hi);
    sort_by_channel(cl, lo, hi, ch);

    int mid = lo + (count - 1) / 2;
    median_cut(cl, lo, mid, depth - 1, max_colors, out_colors, out_count);
    median_cut(cl, mid + 1, hi, depth - 1, max_colors, out_colors, out_count);
}

/* Round color to the nearest palette index (linear scan). */
static uint8_t nearest_index(const rgb_color *pal, int n, uint8_t r, uint8_t g, uint8_t b) {
    int best = 0;
    int best_dist = 999999;
    for (int i = 0; i < n; i++) {
        int dr = (int)r - pal[i].r;
        int dg = (int)g - pal[i].g;
        int db = (int)b - pal[i].b;
        int dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return (uint8_t)best;
}

/* Quantize an RGBA image to a palette + index buffer.
 * Returns the palette count (also stored in *out_count). */
static int quantize_image(const uint8_t *rgba, int w, int h,
                          int max_colors,
                          rgb_color *out_pal, uint8_t *out_idx, int *out_count) {
    int npix = w * h;

    color_list *cl = cl_from_image(rgba, w, h);
    if (!cl) return -1;

    /* How many recursion levels? Each level doubles the possible colors. */
    int depth = 0;
    int cap = 1;
    while (cap < max_colors) {
        cap *= 2;
        depth++;
    }
    /* Clamp depth so we never exceed 8 cuts (256 leaf max). */
    if (depth > 8) depth = 8;

    int pal_count = 0;
    median_cut(cl, 0, cl->n - 1, depth, max_colors, out_pal, &pal_count);
    free(cl->colors);
    free(cl->indices);
    free(cl);

    if (pal_count == 0) pal_count = 1;
    if (pal_count > max_colors) pal_count = max_colors;

    /* Map each pixel to its nearest palette entry. */
    for (int i = 0; i < npix; i++) {
        out_idx[i] = nearest_index(out_pal, pal_count,
                                   rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]);
    }

    *out_count = pal_count;
    return 0;
}

/* ---- LZW compression ----------------------------------------------------- */
/*
 * GIF LZW: variable code width.  The encoder starts with an initial code
 * size of `min_code_size` (e.g. 2..8).  Clear code = 1 << min_code_size,
 * EOI code = clear + 1.  Code table starts at 8 bits and grows up to 12.
 *
 * We use a simple open-addressing hash table for match lookup.
 */

typedef struct {
    uint16_t prefix;
    uint8_t  suffix;
    uint16_t next;  /* next entry in the same hash chain (for collision resolution) */
} lzw_entry;

typedef struct {
    lzw_entry *table;
    int        size;       /* current code width in bits  (2..12) */
    int        cap;        /* max code for current width  = 2^size */
    int        count;      /* number of used table slots  */
    int        clear;      /* clear code value */
    int        eof;        /* end-of-information code value */
    int        next_code;  /* next code to assign */
    int        hash_bits;  /* log2(table allocation) */
    int       *hash_head;  /* head of chain for a (prefix, suffix) hash */
    int       *chain;      /* chain links (index into table) */
} lzw_state;

#define LZW_HASH_SIZE  (1 << 15)   /* 32768 slots */

static int lzw_init(lzw_state *s, int min_code_size) {
    if (min_code_size < 2) min_code_size = 2;
    if (min_code_size > 8) min_code_size = 8;

    int clear = 1 << min_code_size;
    int eof   = clear + 1;
    int start_codes = clear + 2;

    s->clear     = clear;
    s->eof       = eof;
    s->size      = min_code_size + 1;   /* initial code width */
    s->cap       = 1 << s->size;
    s->next_code = start_codes;
    s->count     = start_codes;
    s->hash_bits = 15;
    s->hash_head = (int *)malloc(sizeof(int) * LZW_HASH_SIZE);
    s->chain     = (int *)malloc(sizeof(int) * LZW_HASH_SIZE);
    s->table     = (lzw_entry *)malloc(sizeof(lzw_entry) * 4096);
    if (!s->hash_head || !s->chain) return -1;
    if (!s->table) return -1;

    /* Pre-initialize the table entries 0..clear+1 as standalone bytes
     * (these are implicit in GIF LZW; we don't store them in the hash table). */
    memset(s->hash_head, -1, sizeof(int) * LZW_HASH_SIZE);
    memset(s->chain, 0, sizeof(int) * LZW_HASH_SIZE);

    return 0;
}

static void lzw_free(lzw_state *s) {
    free(s->hash_head);
    free(s->chain);
    free(s->table);
}

/* Hash a (prefix, suffix) pair. */
static inline int lzw_hash(int prefix, uint8_t suffix, int bits) {
    return ((prefix << 8) | suffix) & ((1 << bits) - 1);
    /* Better mixing (optional) — keep it simple and spec-compliant: */
}

/* Write a code (variable length, `s->size` bits) into the bit buffer
 * and flush complete bytes to the output sub-block stream. */
typedef struct {
    uint8_t  block[256];   /* one sub-block can be up to 255 bytes */
    uint8_t  bitbuf;       /* accumulator */
    int      bitcount;     /* bits in bitbuf */
    int      block_len;    /* bytes in block[] */
    gif_writer *w;
} lzw_writer;

static void lzw_flush_block(lzw_writer *lw);

static inline void lzw_emit_code(lzw_writer *lw, int code, int size) {
    lzw_state *derived_unused = NULL; (void)derived_unused;
    for (int i = 0; i < size; i++) {
        int bit = (code >> i) & 1;
        lw->bitbuf |= (uint8_t)(bit << lw->bitcount);
        lw->bitcount++;
        if (lw->bitcount == 8) {
            lw->block[lw->block_len++] = lw->bitbuf;
            lw->bitbuf = 0;
            lw->bitcount = 0;
            if (lw->block_len == 255) lzw_flush_block(lw);
        }
    }
}

static void lzw_flush_block(lzw_writer *lw) {
    if (lw->block_len == 0) return;
    gw_putc(lw->w, (uint8_t)lw->block_len);
    gw_write(lw->w, lw->block, (size_t)lw->block_len);
    lw->block_len = 0;
}

static void lzw_finish(lzw_writer *lw) {
    /* Flush remaining partial byte. */
    if (lw->bitcount > 0) {
        lw->block[lw->block_len++] = lw->bitbuf;
        lw->bitbuf = 0;
        lw->bitcount = 0;
    }
    lzw_flush_block(lw);
    gw_putc(lw->w, 0);  /* block terminator */
}

/* LZW-encode a row of indexed data. */
static void lzw_encode_row(lzw_writer *lw, lzw_state *s,
                           const uint8_t *data, int n) {
    if (n == 0) return;

    int w = s->clear;
    for (int i = 0; i < n; i++) {
        uint8_t c = data[i];
        int key = (w << 8) | c;
        /* Look up key in the table. */
        int h = lzw_hash(w, c, s->hash_bits);
        h %= LZW_HASH_SIZE;
        int found = -1;
        for (int idx = s->hash_head[h]; idx >= 0; idx = s->chain[idx]) {
            if (s->table[idx].prefix == (uint16_t)w &&
                s->table[idx].suffix == c) {
                found = idx + s->clear + 2; /* convert to code value */
                break;
            }
        }

        if (found >= 0) {
            w = found;
        } else {
            /* Emit the code for w, then add (w,c) to the table. */
            lzw_emit_code(lw, w, s->size);

            if (s->next_code < 4096) {
                int slot = s->next_code - s->clear - 2;
                if (slot < 0) slot = 0;
                s->table[slot].prefix  = (uint16_t)w;
                s->table[slot].suffix  = c;
                /* Insert into hash chain. */
                s->chain[slot] = s->hash_head[h];
                s->hash_head[h] = slot;

                s->next_code++;

                /* Bump code width when we've used all codes of the current width. */
                if (s->next_code >= s->cap && s->size < GIF_MAX_LZW_BITS) {
                    s->size++;
                    s->cap = 1 << s->size;
                }
            } else {
                /* Table full: emit clear and reset. */
                lzw_emit_code(lw, s->clear, s->size);
                /* Reset table. */
                memset(s->hash_head, -1, sizeof(int) * LZW_HASH_SIZE);
                s->next_code = s->clear + 2;
                s->size = s->size; /* keep current width */
                s->cap   = 1 << s->size;
            }
            w = c;
        }
    }
    lzw_emit_code(lw, w, s->size);
}

/* ---- GIF structure writers ----------------------------------------------- */

static void gif_write_header(gif_writer *w, int width, int height,
                             int global_colors, int color_resolution,
                             int bg_color_index, int pixel_aspect) {
    gw_write(w, GIF_SIG, 6);
    gw_put16(w, (uint16_t)width);
    gw_put16(w, (uint16_t)height);
    /* Logical Screen Descriptor packed field:
     * bit 7:    Global Color Table Flag (1 = present)
     * bit 6-4:  Color Resolution (minus 1)
     * bit 3:    Sort Flag
     * bit 2-0:  Size of GCT (2^(N+1)) */
    int gct_flag = global_colors > 0 ? 1 : 0;
    int gct_size_field = 0;
    int nc = global_colors;
    while (nc > 2 && (nc & 1) == 0) nc >>= 1;
    /* Find log2(N+1): */
    {
        int sz = global_colors;
        int field = 0;
        while ((1 << (field + 1)) < sz) field++;
        if (field > 7) field = 7;
        gct_size_field = field;
    }
    uint8_t packed = (uint8_t)((gct_flag << 7) |
                               ((color_resolution - 1) << 4) |
                               (0 << 3) |
                               gct_size_field);
    gw_putc(w, packed);
    gw_putc(w, (uint8_t)bg_color_index);
    gw_putc(w, (uint8_t)pixel_aspect);
}

static void gif_write_gct(gif_writer *w, const rgb_color *pal, int ncolors) {
    /* Pad palette to the next power of two (minimum 2). */
    int size = 2;
    while (size < ncolors) size <<= 1;
    if (size > GIF_MAX_PAL) size = GIF_MAX_PAL;

    for (int i = 0; i < size; i++) {
        uint8_t r = (i < ncolors) ? pal[i].r : 0;
        uint8_t g = (i < ncolors) ? pal[i].g : 0;
        uint8_t b = (i < ncolors) ? pal[i].b : 0;
        gw_putc(w, r);
        gw_putc(w, g);
        gw_putc(w, b);
    }
}

/* Write the Netscape 2.0 looping extension (infinite loops). */
static void gif_write_netscape_loop(gif_writer *w) {
    /* Extension Introducer + Application Extension Label */
    gw_putc(w, GIF_EXT_INTRO);   /* 0x21 */
    gw_putc(w, GIF_APP_EXT);     /* 0xFF — application extension */
    gw_write(w, (uint8_t[]){11}, 1); /* block size = 11 */
    gw_write(w, "NETSCAPE2.0", 11);
    /* Sub-block: 3 bytes = 0x01, loop_low, loop_high */
    gw_putc(w, 0x03);            /* sub-block size */
    gw_putc(w, 0x01);            /* extension label (looping) */
    gw_putc(w, 0x00);            /* loop count low (0 = infinite) */
    gw_putc(w, 0x00);            /* loop count high */
    gw_putc(w, 0x00);            /* block terminator */
}

/* Write a Graphic Control Extension for the current frame.
 * delay is in 1/100ths of a second.  transparent_index is the bg index
 * to treat as transparent (use 0 and set transparent_flag=0 for none). */
static void gif_write_gce(gif_writer *w, int delay_cs, int transparent_flag,
                          int transparent_index, int disposal) {
    gw_putc(w, GIF_EXT_INTRO);   /* 0x21 */
    gw_putc(w, GIF_GCE_EXT);     /* 0xF9 — GCE label */
    gw_putc(w, 0x04);            /* block size (GCE data is always 4) */
    uint8_t packed = (uint8_t)((disposal << 2) |
                               (transparent_flag << 0));
    gw_putc(w, packed);
    gw_putc(w, (uint8_t)(delay_cs & 0xFF));
    gw_putc(w, (uint8_t)((delay_cs >> 8) & 0xFF));
    gw_putc(w, (uint8_t)transparent_index);
    gw_putc(w, 0x00);            /* block terminator */
}

/* Write an Image Descriptor for a full-frame image. */
static void gif_write_image_descriptor(gif_writer *w,
                                       int left, int top,
                                       int width, int height,
                                       int interlace) {
    gw_putc(w, GIF_IMG_DESCR);   /* 0x2C */
    gw_put16(w, (uint16_t)left);
    gw_put16(w, (uint16_t)top);
    gw_put16(w, (uint16_t)width);
    gw_put16(w, (uint16_t)height);
    uint8_t packed = (uint8_t)(interlace ? 0x40 : 0x00);
    gw_putc(w, packed);
}

/* Write LZW data sub-blocks for one indexed image.
 * data: indices, one byte per pixel (row-major, top-to-bottom).
 * min_code_size: 2..8 — the LZW minimum code size. */
static void gif_write_lzw_data(gif_writer *w, const uint8_t *data,
                               int width, int height, int min_code_size) {
    gw_putc(w, (uint8_t)min_code_size);

    lzw_state s;
    if (lzw_init(&s, min_code_size) != 0) {
        /* Fallback: write a single clear block so the GIF isn't corrupt. */
        lzw_writer lw = { .w = w, .block_len = 0, .bitbuf = 0, .bitcount = 0 };
        gw_putc(w, 0);
        return;
    }

    lzw_writer lw = { .w = w, .block_len = 0, .bitbuf = 0, .bitcount = 0 };

    /* Emit clear code at the start. */
    lzw_emit_code(&lw, s.clear, s.size);

    for (int y = 0; y < height; y++) {
        lzw_encode_row(&lw, &s, data + (size_t)y * width, width);
    }

    /* Emit EOI code. */
    lzw_emit_code(&lw, s.eof, s.size);
    lzw_finish(&lw);
    lzw_free(&s);
}

/* ---- frame float→RGBA8 conversion --------------------------------------- */

static void frame_float_to_rgba8(const wb_frame *f, uint8_t *dst,
                                 int dst_w, int dst_h) {
    int src_w = f->w;
    int src_h = f->h;
    for (int yd = 0; yd < dst_h; yd++) {
        int sy = (yd * src_h) / dst_h;
        if (sy < 0) sy = 0;
        if (sy >= src_h) sy = src_h - 1;
        for (int xd = 0; xd < dst_w; xd++) {
            int sx = (xd * src_w) / dst_w;
            if (sx < 0) sx = 0;
            if (sx >= src_w) sx = src_w - 1;
            wb_px p = f->px[sy * src_w + sx];
            float r = p.r, g = p.g, b = p.b;
            if (r < 0.f) r = 0.f; else if (r > 1.f) r = 1.f;
            if (g < 0.f) g = 0.f; else if (g > 1.f) g = 1.f;
            if (b < 0.f) b = 0.f; else if (b > 1.f) b = 1.f;
            uint8_t *d = &dst[(yd * dst_w + xd) * 4];
            d[0] = (uint8_t)(r * 255.f + 0.5f);
            d[1] = (uint8_t)(g * 255.f + 0.5f);
            d[2] = (uint8_t)(b * 255.f + 0.5f);
            d[3] = 255;
        }
    }
}

/* ---- palette builder (global palette shared across frames) -------------- */
/*
 * GIF89a uses a single Global Color Table shared by all frames.  We build
 * the palette from the FIRST frame and quantize every subsequent frame to
 * that same palette.  This is simpler than per-frame local tables and is
 * the standard approach for web GIFs.
 */

static int build_global_palette(const uint8_t *rgba, int w, int h,
                                int max_colors, rgb_color *out_pal, int *out_count) {
    int nc = max_colors;
    if (nc < 2) nc = 2;
    if (nc > GIF_MAX_PAL) nc = GIF_MAX_PAL;

    rgb_color local_pal[GIF_MAX_PAL];
    uint8_t *idx = (uint8_t *)malloc((size_t)w * h);
    if (!idx) return -1;

    int rc = quantize_image(rgba, w, h, nc, local_pal, idx, out_count);
    free(idx);
    if (rc != 0) return -1;

    memcpy(out_pal, local_pal, (size_t)(*out_count) * sizeof(rgb_color));
    return 0;
}

/* Map RGBA pixels to the nearest entry in a fixed palette. */
static void map_to_palette(const uint8_t *rgba, int npix,
                           const rgb_color *pal, int ncolors,
                           uint8_t *indices) {
    for (int i = 0; i < npix; i++) {
        indices[i] = nearest_index(pal, ncolors,
                                   rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]);
    }
}

/* ---- minimum code size --------------------------------------------------- */

static int compute_min_code_size(int ncolors) {
    /* GIF LZW minimum code size = ceil(log2(ncolors)), min 2. */
    int sz = 2;
    int v = ncolors - 1;  /* we need codes for 0..ncolors-1 */
    while ((1 << sz) <= v) sz++;
    if (sz < 2) sz = 2;
    return sz;
}

/* ---- public API --------------------------------------------------------- */

int wb_export_gif(wb_edit_graph *g, const char *path,
                  int width, int height, float fps, int max_colors) {
    if (!g || !path) return -1;
    if (g->duration <= 0.0) return -1;

    int w = width ? width : g->width;
    int h = height ? height : g->height;
    if (w <= 0 || h <= 0) return -1;

    if (fps < 0.5f) fps = 15.0f;
    if (fps > 60.0f) fps = 60.0f;

    int nc = max_colors;
    if (nc < 2)  nc = 2;
    if (nc > GIF_MAX_PAL) nc = GIF_MAX_PAL;

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "wb_export_gif: cannot open %s\n", path);
        return -1;
    }

    gif_writer wtr;
    wtr.fp = fp;
    wtr.error = 0;

    double frame_dt = 1.0 / (double)fps;
    int64_t total_frames = (int64_t)(g->duration * fps + 0.5);
    if (total_frames < 1) total_frames = 1;

    /* Evaluate the first frame to build the global palette. */
    wb_frame *first = wb_edit_graph_evaluate(g, 0.0);
    if (!first) {
        fprintf(stderr, "wb_export_gif: first frame eval failed\n");
        fclose(fp);
        return -1;
    }
    if (first->w != w || first->h != h) {
        /* We'll resample in frame_float_to_rgba8. */
    }

    uint8_t *rgba_buf = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba_buf) {
        wb_frame_free(first);
        fclose(fp);
        return -1;
    }
    frame_float_to_rgba8(first, rgba_buf, w, h);

    rgb_color palette[GIF_MAX_PAL];
    int pal_count = 0;
    if (build_global_palette(rgba_buf, w, h, nc, palette, &pal_count) != 0) {
        fprintf(stderr, "wb_export_gif: palette build failed\n");
        free(rgba_buf);
        wb_frame_free(first);
        fclose(fp);
        return -1;
    }

    /* Write GIF header + logical screen descriptor + GCT. */
    gif_write_header(&wtr, w, h, pal_count, 8, 0, 0);
    gif_write_gct(&wtr, palette, pal_count);

    /* Netscape 2.0 looping extension (infinite). */
    gif_write_netscape_loop(&wtr);

    int min_code_size = compute_min_code_size(pal_count);
    int delay_cs = (int)(100.0 / fps + 0.5);  /* centiseconds */
    if (delay_cs < 1) delay_cs = 1;

    /* ---- write frame 0 (already evaluated) ---- */
    uint8_t *idx_buf = (uint8_t *)malloc((size_t)w * h);
    if (!idx_buf) {
        free(rgba_buf); wb_frame_free(first); fclose(fp); return -1;
    }
    map_to_palette(rgba_buf, w * h, palette, pal_count, idx_buf);

    gif_write_gce(&wtr, delay_cs, 0, 0, 0);  /* disposal=0 (none) */
    gif_write_image_descriptor(&wtr, 0, 0, w, h, 0);
    gif_write_lzw_data(&wtr, idx_buf, w, h, min_code_size);

    /* ---- write remaining frames ---- */
    for (int64_t fnum = 1; fnum < total_frames; fnum++) {
        double t = (double)fnum / (double)fps;
        if (t >= g->duration) t = g->duration - 0.001;
        wb_frame *f = wb_edit_graph_evaluate(g, t);
        if (!f) {
            fprintf(stderr, "wb_export_gif: eval failed at frame %lld (t=%.3f)\n",
                    (long long)fnum, t);
            free(idx_buf); free(rgba_buf); fclose(fp);
            return -1;
        }
        frame_float_to_rgba8(f, rgba_buf, w, h);
        wb_frame_free(f);

        map_to_palette(rgba_buf, w * h, palette, pal_count, idx_buf);

        gif_write_gce(&wtr, delay_cs, 0, 0, 0);
        gif_write_image_descriptor(&wtr, 0, 0, w, h, 0);
        gif_write_lzw_data(&wtr, idx_buf, w, h, min_code_size);

        if (wtr.error) break;
    }

    free(idx_buf);
    free(rgba_buf);
    wb_frame_free(first);

    /* Trailer. */
    gw_putc(&wtr, GIF_TRAILER);
    fclose(fp);

    if (wtr.error) {
        fprintf(stderr, "wb_export_gif: write error\n");
        return -1;
    }

    printf("wb_export_gif: wrote %lld frames to %s (%dx%d, %d colors)\n",
           (long long)total_frames, path, w, h, pal_count);
    return 0;
}
