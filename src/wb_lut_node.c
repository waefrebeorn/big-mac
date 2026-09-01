/* wb_lut_node.c — 3D LUT color grading node (R085).
 *
 * Wraps wb_lut3d_load/wb_lut3d_apply as a compositor effect node.
 * Loads .cube LUT files for cinematic color grading.
 */

#include "wbus/wbus_compositor.h"
#include "wbus/wbus_vfx.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    wb_lut3d lut;
    int loaded;
    char path[512];
    float intensity;  /* 0..1 blend between original and graded */
} lut_node_t;

static wb_frame *lut_pull(wb_node *self, double t,
                           int rx, int ry, int rw, int rh, int phase) {
    (void)t;
    lut_node_t *ln = self->user;
    if (self->n_inputs < 1) return NULL;
    if (phase == 0) {
        wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh);
        return NULL;
    }
    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in || !ln->loaded || ln->intensity <= 0.0f) return in;

    int w = in->w, h = in->h;

    /* Convert float to uint8, apply LUT, convert back */
    uint8_t *rgba = malloc(w * h * 4);
    for (int i = 0; i < w * h; i++) {
        rgba[i*4+0] = (uint8_t)(in->px[i].r * 255.0f + 0.5f);
        rgba[i*4+1] = (uint8_t)(in->px[i].g * 255.0f + 0.5f);
        rgba[i*4+2] = (uint8_t)(in->px[i].b * 255.0f + 0.5f);
        rgba[i*4+3] = (uint8_t)(in->px[i].a * 255.0f + 0.5f);
    }

    /* Apply LUT */
    wb_lut3d_apply(&ln->lut, rgba, w * h);

    /* Blend based on intensity */
    for (int i = 0; i < w * h; i++) {
        float orig_r = in->px[i].r * 255.0f;
        float orig_g = in->px[i].g * 255.0f;
        float orig_b = in->px[i].b * 255.0f;
        float graded_r = rgba[i*4+0];
        float graded_g = rgba[i*4+1];
        float graded_b = rgba[i*4+2];
        in->px[i].r = (orig_r * (1.0f - ln->intensity) + graded_r * ln->intensity) / 255.0f;
        in->px[i].g = (orig_g * (1.0f - ln->intensity) + graded_g * ln->intensity) / 255.0f;
        in->px[i].b = (orig_b * (1.0f - ln->intensity) + graded_b * ln->intensity) / 255.0f;
    }

    free(rgba);
    return in;
}

static void lut_free(wb_node *n) { free(n->user); }

wb_node *wb_node_effect_lut(const char *path) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "lut");
    if (!n) return NULL;
    lut_node_t *ln = calloc(1, sizeof(*ln));
    if (!ln) { wb_node_destroy(n); return NULL; }
    ln->intensity = 1.0f;
    if (path && path[0]) {
        snprintf(ln->path, sizeof(ln->path), "%s", path);
        if (wb_lut3d_load(&ln->lut, path) == 0) {
            ln->loaded = 1;
        }
    }
    n->user = ln;
    n->pull = lut_pull;
    n->free = lut_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    return n;
}

void wb_node_effect_lut_set_intensity(wb_node *n, float intensity) {
    if (!n || n->kind != WB_NODE_EFFECT) return;
    lut_node_t *ln = n->user;
    if (ln) ln->intensity = intensity < 0 ? 0 : (intensity > 1 ? 1 : intensity);
}

int wb_node_effect_lut_load(wb_node *n, const char *path) {
    if (!n || n->kind != WB_NODE_EFFECT || !path) return -1;
    lut_node_t *ln = n->user;
    if (!ln) return -1;
    snprintf(ln->path, sizeof(ln->path), "%s", path);
    ln->loaded = (wb_lut3d_load(&ln->lut, path) == 0);
    return ln->loaded ? 0 : -1;
}
