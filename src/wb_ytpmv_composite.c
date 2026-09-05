/* wb_ytpmv_composite.c — YTPMV multi-layer video compositing engine.
 *
 * Handles chroma key cutout, overlay with positioning, opacity/blend modes,
 * picture-in-picture, split screen. Generates ffmpeg filter strings for
 * compositing operations. Pure C11, opaque style.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include "wbus/wbus_compositor.h"

/* ---- Lifecycle ---- */
wb_composite_ctx *wb_composite_create(int w, int h) {
    wb_composite_ctx *ctx = (wb_composite_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->width = w;
    ctx->height = h;
    ctx->n_layers = 0;
    ctx->bg_color[0] = '\0';
    ctx->filter_str[0] = '\0';
    return ctx;
}

void wb_composite_destroy(wb_composite_ctx *ctx) {
    if (ctx) free(ctx);
}

/* ---- Layer management ---- */
int wb_composite_add_layer(wb_composite_ctx *ctx, const char *name,
                            int type) {
    if (!ctx || ctx->n_layers >= WB_COMP_MAX_LAYERS) return -1;
    wb_comp_layer *l = &ctx->layers[ctx->n_layers];
    memset(l, 0, sizeof(*l));
    strncpy(l->name, name, sizeof(l->name) - 1);
    l->type = type;
    l->opacity = 1.0f;
    l->blend = WB_COMP_BLEND_OVER;
    l->visible = 1;
    l->width = 0;
    l->height = 0;
    return ctx->n_layers++;
}

int wb_composite_add_video_layer(wb_composite_ctx *ctx, const char *name,
                                  const char *path) {
    int idx = wb_composite_add_layer(ctx, name, WB_LAYER_VIDEO);
    if (idx < 0) return idx;
    strncpy(ctx->layers[idx].source_path, path, sizeof(ctx->layers[idx].source_path) - 1);
    return idx;
}

int wb_composite_add_color_layer(wb_composite_ctx *ctx, const char *name,
                                  float r, float g, float b, float a) {
    int idx = wb_composite_add_layer(ctx, name, WB_LAYER_SOLID_COLOR);
    if (idx < 0) return idx;
    /* Encode color as a special path marker: color:R,G,B,A */
    snprintf(ctx->layers[idx].source_path, sizeof(ctx->layers[idx].source_path),
             "color:%.3f,%.3f,%.3f,%.3f", r, g, b, a);
    return idx;
}

/* ---- Layer properties ---- */
void wb_layer_set_position(wb_composite_ctx *ctx, int idx, int x, int y) {
    if (!ctx || idx < 0 || idx >= ctx->n_layers) return;
    ctx->layers[idx].x = x;
    ctx->layers[idx].y = y;
}

void wb_layer_set_size(wb_composite_ctx *ctx, int idx, int w, int h) {
    if (!ctx || idx < 0 || idx >= ctx->n_layers) return;
    ctx->layers[idx].width = w;
    ctx->layers[idx].height = h;
}

void wb_layer_set_opacity(wb_composite_ctx *ctx, int idx, float opacity) {
    if (!ctx || idx < 0 || idx >= ctx->n_layers) return;
    ctx->layers[idx].opacity = opacity < 0 ? 0 : (opacity > 1 ? 1 : opacity);
}

void wb_layer_set_blend(wb_composite_ctx *ctx, int idx, wb_comp_blend_mode mode) {
    if (!ctx || idx < 0 || idx >= ctx->n_layers) return;
    ctx->layers[idx].blend = mode;
}

void wb_layer_set_visible(wb_composite_ctx *ctx, int idx, int visible) {
    if (!ctx || idx < 0 || idx >= ctx->n_layers) return;
    ctx->layers[idx].visible = visible;
}

/* ---- Chroma key ---- */
void wb_layer_set_chromakey(wb_composite_ctx *ctx, int idx,
                             float r, float g, float b,
                             float threshold, float softness) {
    if (!ctx || idx < 0 || idx >= ctx->n_layers) return;
    wb_comp_layer *l = &ctx->layers[idx];
    l->use_chromakey = 1;
    l->key_r = r;
    l->key_g = g;
    l->key_b = b;
    l->key_threshold = threshold;
    l->key_softness = softness;
}

/* ---- Blend mode to ffmpeg format ---- */
static const char *blend_mode_name(wb_comp_blend_mode mode) {
    switch (mode) {
    case WB_COMP_BLEND_OVER:     return "over";
    case WB_COMP_BLEND_ADD:      return "addition";
    case WB_COMP_BLEND_MULTIPLY: return "multiply";
    case WB_COMP_BLEND_SCREEN:   return "screen";
    case WB_COMP_BLEND_OVERLAY:  return "overlay";
    default:                     return "over";
    }
}

/* ---- Generate chroma key filter string for a single layer ---- */
static int chromakey_filter(wb_comp_layer *l, char *out, int max_len) {
    /* Convert float RGB to 0xRRGGBB */
    int ri = (int)(l->key_r * 255.0f) & 0xFF;
    int gi = (int)(l->key_g * 255.0f) & 0xFF;
    int bi = (int)(l->key_b * 255.0f) & 0xFF;
    uint32_t color_val = (uint32_t)((ri << 16) | (gi << 8) | bi);

    return snprintf(out, max_len,
        "colorkey=Color=%06x:Similarity=%.3f:Blend=%.3f",
        color_val, l->key_threshold, l->key_softness);
}

/* ---- Generate scale filter string ---- */
static int scale_filter(wb_comp_layer *l, int base_w, int base_h, char *out, int max_len) {
    if (l->width > 0 && l->height > 0) {
        return snprintf(out, max_len, "scale=%d:%d", l->width, l->height);
    }
    return snprintf(out, max_len, "scale=%d:%d", base_w, base_h);
}

/* ---- Generate overlay position string ---- */
static int overlay_position(wb_comp_layer *l, char *out, int max_len) {
    if (l->x == 0 && l->y == 0) {
        return snprintf(out, max_len, "0");
    }
    if (l->y == 0) {
        return snprintf(out, max_len, "%d", l->x);
    }
    return snprintf(out, max_len, "%d:%d", l->x, l->y);
}

/* ---- Generate the full ffmpeg filter_complex string ---- */
int wb_composite_generate_filter(wb_composite_ctx *ctx) {
    if (!ctx || ctx->n_layers == 0) return -1;

    char *buf = ctx->filter_str;
    int total = WB_COMP_MAX_FILTER_LEN;
    int pos = 0;

    /* Find visible layers */
    int visible_count = 0;
    int visible_indices[WB_COMP_MAX_LAYERS];
    for (int i = 0; i < ctx->n_layers; i++) {
        if (ctx->layers[i].visible) {
            visible_indices[visible_count++] = i;
        }
    }
    if (visible_count == 0) return -1;

    /* Single layer: just chromakey if needed */
    if (visible_count == 1) {
        wb_comp_layer *l = &ctx->layers[visible_indices[0]];
        if (l->use_chromakey) {
            pos += snprintf(buf + pos, total - pos, "[0:v]");
            char key[256];
            chromakey_filter(l, key, sizeof(key));
            pos += snprintf(buf + pos, total - pos, "%s", key);
        }
        ctx->filter_str[pos] = '\0';
        return 0;
    }

    /* Multi-layer: chain overlays */
    int base = visible_indices[0];

    /* Start with base layer label */
    pos += snprintf(buf + pos, total - pos, "[0:v]");

    /* Apply chromakey to base if needed */
    if (ctx->layers[base].use_chromakey) {
        char key[256];
        chromakey_filter(&ctx->layers[base], key, sizeof(key));
        pos += snprintf(buf + pos, total - pos, "%s", key);
    }
    pos += snprintf(buf + pos, total - pos, "[base];");

    /* Chain overlay for each subsequent layer */
    for (int i = 1; i < visible_count; i++) {
        int li = visible_indices[i];
        wb_comp_layer *l = &ctx->layers[li];

        /* Input label for this layer */
        pos += snprintf(buf + pos, total - pos, "[%d:v]", i);

        /* Scale if needed */
        char scale[128];
        scale_filter(l, ctx->width, ctx->height, scale, sizeof(scale));
        if (strcmp(scale, "") != 0) {
            pos += snprintf(buf + pos, total - pos, "%s,", scale);
        }

        /* Chromakey if needed */
        if (l->use_chromakey) {
            char key[256];
            chromakey_filter(l, key, sizeof(key));
            pos += snprintf(buf + pos, total - pos, "%s,", key);
        }

        /* Opacity via format + colorchannelmixer */
        if (l->opacity < 1.0f) {
            pos += snprintf(buf + pos, total - pos,
                "format=rgba,colorchannelmixer=aa=%.3f,", l->opacity);
        }

        /* Remove trailing comma */
        if (pos > 0 && buf[pos - 1] == ',') pos--;

        /* Label this processed layer */
        pos += snprintf(buf + pos, total - pos, "[l%d];", li);

        /* Overlay */
        char ov_pos[64];
        overlay_position(l, ov_pos, sizeof(ov_pos));

        const char *blend_name = blend_mode_name(l->blend);
        if (i == 1) {
            pos += snprintf(buf + pos, total - pos,
                "[base][l%d]overlay=%s:format=auto,shortest=1",
                li, ov_pos);
        } else {
            pos += snprintf(buf + pos, total - pos,
                "[out_prev][l%d]overlay=%s:format=auto",
                li, ov_pos);
        }

        /* Blend mode (via blend filter if not 'over') */
        if (l->blend != WB_COMP_BLEND_OVER) {
            pos += snprintf(buf + pos, total - pos,
                ",blend=%s", blend_name);
        }

        if (i < visible_count - 1) {
            pos += snprintf(buf + pos, total - pos, "[out_prev];");
        }
    }

    ctx->filter_str[pos] = '\0';
    return 0;
}

/* ---- Picture-in-picture preset ---- */
int wb_composite_pip(wb_composite_ctx *ctx, int main_idx, int pip_idx,
                      int pip_x, int pip_y, int pip_w, int pip_h) {
    if (!ctx || main_idx < 0 || main_idx >= ctx->n_layers) return -1;
    if (pip_idx < 0 || pip_idx >= ctx->n_layers) return -1;

    wb_layer_set_position(ctx, pip_idx, pip_x, pip_y);
    wb_layer_set_size(ctx, pip_idx, pip_w, pip_h);
    return wb_composite_generate_filter(ctx);
}

/* ---- Split screen preset ---- */
int wb_composite_split_screen(wb_composite_ctx *ctx, int left_idx, int right_idx,
                               int vertical) {
    if (!ctx || left_idx < 0 || left_idx >= ctx->n_layers) return -1;
    if (right_idx < 0 || right_idx >= ctx->n_layers) return -1;

    int half_w = ctx->width / 2;
    int half_h = ctx->height / 2;

    if (vertical) {
        /* Left half and right half */
        wb_layer_set_position(ctx, left_idx, 0, 0);
        wb_layer_set_size(ctx, left_idx, half_w, ctx->height);
        wb_layer_set_position(ctx, right_idx, half_w, 0);
        wb_layer_set_size(ctx, right_idx, half_w, ctx->height);
    } else {
        /* Top half and bottom half */
        wb_layer_set_position(ctx, left_idx, 0, 0);
        wb_layer_set_size(ctx, left_idx, ctx->width, half_h);
        wb_layer_set_position(ctx, right_idx, 0, half_h);
        wb_layer_set_size(ctx, right_idx, ctx->width, half_h);
    }
    return wb_composite_generate_filter(ctx);
}

/* ---- Accessors ---- */
const char *wb_composite_get_filter(const wb_composite_ctx *ctx) {
    return ctx ? ctx->filter_str : "";
}

int wb_composite_get_layer_count(const wb_composite_ctx *ctx) {
    return ctx ? ctx->n_layers : 0;
}

const wb_comp_layer *wb_composite_get_layer(const wb_composite_ctx *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->n_layers) return NULL;
    return &ctx->layers[idx];
}

/* ---- Generate full ffmpeg command ---- */
int wb_composite_generate_cmd(const wb_composite_ctx *ctx, char *buf, int max_len) {
    if (!ctx || !buf || max_len <= 0) return -1;
    return snprintf(buf, max_len,
        "ffmpeg -i input0.mp4 -i input1.mp4 -filter_complex \"%s\" -c:a copy output.mp4",
        ctx->filter_str);
}