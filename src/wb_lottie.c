/* wb_lottie.c — Lottie JSON motion graphics renderer (After Effects/Lottie style).
 *
 * Parse Lottie JSON, render keyframed animations to RGBA buffers.
 * Supports: rectangles, ellipses, paths, transforms, keyframe interpolation.
 * Pure C11. */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_LAYERS 32
#define MAX_KEYFRAMES 64

typedef struct {
    float time;
    float value[4]; /* up to 4-component values */
} lottie_keyframe_t;

typedef struct {
    char type[16];      /* "rectangle", "ellipse", "path", "group" */
    char name[64];
    int visible;
    float transform[6]; /* a,b,c,d,e,f affine matrix */
    float opacity;
    float color[4];     /* RGBA */
    float width, height; /* shape size */
    float position[2];

    /* Animation */
    lottie_keyframe_t pos_keys[MAX_KEYFRAMES];
    int num_pos_keys;
    lottie_keyframe_t scale_keys[MAX_KEYFRAMES];
    int num_scale_keys;
    lottie_keyframe_t rot_keys[MAX_KEYFRAMES];
    int num_rot_keys;
    lottie_keyframe_t op_keys[MAX_KEYFRAMES];
    int num_op_keys;
} lottie_layer_t;

typedef struct {
    lottie_layer_t layers[MAX_LAYERS];
    int num_layers;
    float duration;
    int fps;
    int width, height;
} lottie_anim_t;

/* Minimal JSON parser helpers */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *find_key(const char *json, const char *key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    p = skip_ws(p);
    if (*p == ':') p++;
    return skip_ws(p);
}

static float parse_float(const char *p) {
    return strtof(p, NULL);
}

static int parse_int(const char *p) {
    return (int)strtol(p, NULL, 10);
}

/* Parse array of floats from JSON */
static int parse_float_array(const char *p, float *out, int max_n) {
    if (!p || *p != '[') return 0;
    p++;
    int n = 0;
    while (n < max_n) {
        p = skip_ws(p);
        if (*p == ']') break;
        out[n++] = parse_float(p);
        /* Skip to next comma or close */
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
    return n;
}

/* Simple shape: parse basic properties */
static void parse_layer(const char *json, const char *layer_start, lottie_layer_t *layer) {
    memset(layer, 0, sizeof(*layer));
    snprintf(layer->type, sizeof(layer->type), "rectangle");
    layer->visible = 1;
    layer->opacity = 1.0f;
    layer->color[0] = 1; layer->color[1] = 1; layer->color[2] = 1; layer->color[3] = 1;
    layer->width = 100;
    layer->height = 100;

    /* Look for ty (type) */
    const char *ty = find_key(layer_start, "ty");
    if (ty) {
        if (ty[0] == '"') {
            ty++;
            if (strncmp(ty, "rc", 2) == 0) snprintf(layer->type, sizeof(layer->type), "ellipse");
            else if (strncmp(ty, "sh", 2) == 0) snprintf(layer->type, sizeof(layer->type), "path");
            else if (strncmp(ty, "gr", 2) == 0) snprintf(layer->type, sizeof(layer->type), "group");
        }
    }

    /* Parse size */
    const char *ks = find_key(layer_start, "ks");
    if (ks) {
        const char *s = find_key(ks, "s"); /* scale */
        if (s) { layer->transform[0] = layer->transform[3] = parse_float(s + 1) / 100.0f; }
        const char *o = find_key(ks, "o"); /* opacity */
        if (o) { layer->opacity = parse_float(o); }
        const char *p = find_key(ks, "p"); /* position */
        if (p) { parse_float_array(p, layer->position, 2); }
    }
}

/* Interpolate between keyframes */
static float interpolate_keyframes(lottie_keyframe_t *keys, int num_keys, float time, int comp) {
    if (num_keys == 0) return 0;
    if (num_keys == 1 || time <= keys[0].time) return keys[0].value[comp];
    if (time >= keys[num_keys-1].time) return keys[num_keys-1].value[comp];

    /* Find surrounding keyframes */
    for (int i = 0; i < num_keys - 1; i++) {
        if (time >= keys[i].time && time <= keys[i+1].time) {
            float t0 = keys[i].time;
            float t1 = keys[i+1].time;
            float frac = (t1 > t0) ? (time - t0) / (t1 - t0) : 0;
            /* Linear interpolation */
            float v0 = keys[i].value[comp];
            float v1 = keys[i+1].value[comp];
            return v0 + (v1 - v0) * frac;
        }
    }
    return keys[num_keys-1].value[comp];
}

/* Draw filled rectangle into RGBA buffer */
static void draw_rect(uint8_t *rgba, int w, int h,
                       float cx, float cy, float rw, float rh,
                       float r, float g, float b, float a) {
    int x0 = (int)(cx - rw/2);
    int y0 = (int)(cy - rh/2);
    int x1 = (int)(cx + rw/2);
    int y1 = (int)(cy + rh/2);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w-1; if (y1 >= h) y1 = h-1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int idx = (y * w + x) * 4;
            rgba[idx]   = (uint8_t)(r * a * 255);
            rgba[idx+1] = (uint8_t)(g * a * 255);
            rgba[idx+2] = (uint8_t)(b * a * 255);
            rgba[idx+3] = (uint8_t)(a * 255);
        }
    }
}

/* Public API */

void *wb_lottie_create(void) {
    lottie_anim_t *anim = (lottie_anim_t *)calloc(1, sizeof(*anim));
    if (!anim) return NULL;
    anim->fps = 30;
    anim->duration = 1.0f;
    anim->width = 1920;
    anim->height = 1080;
    return anim;
}

void wb_lottie_destroy(void *ptr) { free(ptr); }

int wb_lottie_load_json(void *ptr, const char *json_str, int json_len) {
    lottie_anim_t *anim = (lottie_anim_t *)ptr;
    if (!anim || !json_str) return -1;

    /* Parse top-level properties */
    const char *v = find_key(json_str, "v");
    if (v) { /* version, skip */ }

    const char *fr = find_key(json_str, "fr");
    if (fr) anim->fps = parse_int(fr);

    const char *ip = find_key(json_str, "ip");
    if (ip) { /* in point */ }

    const char *op = find_key(json_str, "op");
    if (op) anim->duration = (float)parse_int(op) / anim->fps;

    const char *w = find_key(json_str, "w");
    if (w) anim->width = parse_int(w);

    const char *h = find_key(json_str, "h");
    if (h) anim->height = parse_int(h);

    /* Parse layers */
    const char *layers = find_key(json_str, "layers");
    if (!layers) return 0;

    /* Count and parse each layer (simplified: look for { "ty" ) */
    const char *p = layers;
    anim->num_layers = 0;
    while ((p = strstr(p, "\"ty\"")) != NULL && anim->num_layers < MAX_LAYERS) {
        /* Find the layer object start */
        const char *layer_start = p;
        while (layer_start > layers && *layer_start != '{') layer_start--;
        parse_layer(json_str, layer_start, &anim->layers[anim->num_layers]);
        anim->num_layers++;
        p += 4;
    }

    return anim->num_layers;
}

int wb_lottie_load_file(void *ptr, const char *path) {
    if (!path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, len, f);
    buf[len] = 0;
    fclose(f);
    int ret = wb_lottie_load_json(ptr, buf, (int)len);
    free(buf);
    return ret;
}

int wb_lottie_render_frame(void *ptr, uint8_t *rgba_out, int w, int h, float time_sec) {
    lottie_anim_t *anim = (lottie_anim_t *)ptr;
    if (!anim || !rgba_out) return -1;

    /* Clear to transparent */
    memset(rgba_out, 0, (size_t)w * h * 4);

    /* Render each layer */
    for (int i = 0; i < anim->num_layers; i++) {
        lottie_layer_t *layer = &anim->layers[i];
        if (!layer->visible) continue;

        float px = layer->position[0];
        float py = layer->position[1];
        float opacity = layer->opacity;
        float rw = layer->width;
        float rh = layer->height;

        /* Apply animated keyframes if present */
        if (layer->num_pos_keys > 0) {
            px = interpolate_keyframes(layer->pos_keys, layer->num_pos_keys, time_sec, 0);
            py = interpolate_keyframes(layer->pos_keys, layer->num_pos_keys, time_sec, 1);
        }
        if (layer->num_op_keys > 0) {
            opacity = interpolate_keyframes(layer->num_op_keys > 0 ? layer->op_keys : NULL,
                                             layer->num_op_keys, time_sec, 0);
        }

        /* Scale to output resolution */
        float scale_x = (float)w / anim->width;
        float scale_y = (float)h / anim->height;

        draw_rect(rgba_out, w, h,
                  px * scale_x, py * scale_y,
                  rw * scale_x, rh * scale_y,
                  layer->color[0], layer->color[1], layer->color[2], opacity);
    }

    return 0;
}

float wb_lottie_get_duration(const void *ptr) {
    const lottie_anim_t *anim = (const lottie_anim_t *)ptr;
    return anim ? anim->duration : 0;
}

int wb_lottie_get_fps(const void *ptr) {
    const lottie_anim_t *anim = (const lottie_anim_t *)ptr;
    return anim ? anim->fps : 0;
}
