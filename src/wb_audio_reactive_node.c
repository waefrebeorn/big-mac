/* wb_audio_reactive_node.c — audio-reactive video FX node (R084).
 *
 * Wraps wb_audio_reactive.c as a compositor effect node. On pull:
 *   1. Pulls the input frame
 *   2. Reads the current audio features from the global audio-reactive state
 *   3. Applies brightness, color shift, flash, shake offset
 *   4. Returns the modified frame
 *
 * The audio side is fed by the engine's audio callback (wb_audio_reactive_update).
 * This node just reads the computed visual params at pull time.
 */

#include "wbus/wbus_compositor.h"
#include "wbus/wbus_vfx.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* From wb_audio_reactive.c (declared in wbus.h) */
typedef struct {
    float out_zoom, out_flash, out_shake, out_color_shift, out_brightness;
} wb_audio_reactive_outputs;

/* Global audio-reactive state (set by the engine's audio callback) */
static wb_audio_reactive_outputs g_ar_outputs = {0};

/* Called by the engine each audio block to update visual params. */
void wb_audio_reactive_node_set_params(float zoom, float flash, float shake,
                                        float color_shift, float brightness) {
    g_ar_outputs.out_zoom = zoom;
    g_ar_outputs.out_flash = flash;
    g_ar_outputs.out_shake = shake;
    g_ar_outputs.out_color_shift = color_shift;
    g_ar_outputs.out_brightness = brightness;
}

/* Get current audio-reactive outputs (for UI readouts). */
void wb_audio_reactive_node_get_params(float *zoom, float *flash, float *shake,
                                        float *color_shift, float *brightness) {
    if (zoom) *zoom = g_ar_outputs.out_zoom;
    if (flash) *flash = g_ar_outputs.out_flash;
    if (shake) *shake = g_ar_outputs.out_shake;
    if (color_shift) *color_shift = g_ar_outputs.out_color_shift;
    if (brightness) *brightness = g_ar_outputs.out_brightness;
}

/* Audio-reactive effect node state */
typedef struct {
    float intensity;       /* 0..2 multiplier for effect strength */
    int   enable_flash;    /* beat flash */
    int   enable_shake;    /* energy shake */
    int   enable_zoom;     /* bass zoom */
    int   enable_color;    /* spectral color shift */
} ar_node_t;

static wb_frame *ar_node_pull(wb_node *self, double t,
                               int rx, int ry, int rw, int rh, int phase) {
    (void)t;
    ar_node_t *ar = self->user;
    if (self->n_inputs < 1) return NULL;

    /* Phase 0: request upstream */
    if (phase == 0) {
        wb_node_pull_request(self->inputs[0], t, rx, ry, rw, rh);
        return NULL;
    }

    wb_frame *in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;

    float flash = ar->enable_flash ? g_ar_outputs.out_flash * ar->intensity : 0.0f;
    float brightness = ar->enable_color ? (0.7f + g_ar_outputs.out_brightness * 0.5f * ar->intensity) : 1.0f;
    float color_shift = ar->enable_color ? g_ar_outputs.out_color_shift * 360.0f * ar->intensity : 0.0f;
    float shake_x = 0, shake_y = 0;

    if (ar->enable_shake && g_ar_outputs.out_shake > 0.01f) {
        /* Deterministic pseudo-random shake based on time */
        float amt = g_ar_outputs.out_shake * ar->intensity * 8.0f;
        shake_x = sinf((float)(t * 123.456f)) * amt;
        shake_y = cosf((float)(t * 789.012f)) * amt;
    }

    /* Apply brightness + color shift + flash */
    for (int y = in->roi_y; y < in->roi_y + in->roi_h; y++) {
        for (int x = in->roi_x; x < in->roi_x + in->roi_w; x++) {
            /* Shake offset */
            int sx = (int)(x + shake_x);
            int sy = (int)(y + shake_y);
            if (sx < 0) sx = 0; if (sx >= in->w) sx = in->w - 1;
            if (sy < 0) sy = 0; if (sy >= in->h) sy = in->h - 1;

            wb_px *src = &in->px[sy * in->w + sx];
            wb_px *dst = &in->px[y * in->w + x];

            /* Copy with shake */
            dst->r = src->r;
            dst->g = src->g;
            dst->b = src->b;
            dst->a = src->a;

            /* Brightness */
            dst->r *= brightness;
            dst->g *= brightness;
            dst->b *= brightness;

            /* Color shift (warm/cool) */
            if (color_shift > 1.0f) {
                float shift = (color_shift - 1.0f) * 0.3f * ar->intensity;
                dst->r += shift * 0.5f;
                dst->b -= shift * 0.3f;
            }

            /* Flash overlay */
            if (flash > 0.01f) {
                dst->r += flash * (1.0f - dst->r);
                dst->g += flash * (1.0f - dst->g);
                dst->b += flash * (1.0f - dst->b);
            }

            /* Clamp */
            if (dst->r > 1.0f) dst->r = 1.0f; if (dst->r < 0) dst->r = 0;
            if (dst->g > 1.0f) dst->g = 1.0f; if (dst->g < 0) dst->g = 0;
            if (dst->b > 1.0f) dst->b = 1.0f; if (dst->b < 0) dst->b = 0;
        }
    }

    return in;
}

static void ar_node_free(wb_node *n) { free(n->user); }

wb_node *wb_node_effect_audio_reactive(float intensity) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, "audio_reactive");
    if (!n) return NULL;
    ar_node_t *ar = calloc(1, sizeof(*ar));
    if (!ar) { wb_node_destroy(n); return NULL; }
    ar->intensity = intensity > 0 ? intensity : 1.0f;
    ar->enable_flash = 1;
    ar->enable_shake = 1;
    ar->enable_zoom = 1;
    ar->enable_color = 1;
    n->user = ar;
    n->pull = ar_node_pull;
    n->free = ar_node_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    return n;
}
