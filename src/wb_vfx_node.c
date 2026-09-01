/* wb_vfx_node.c — wrap wb_vfx.c effects as compositor effect nodes (R077).
 *
 * Each factory creates a WB_NODE_EFFECT node with one input. The pull
 * function reads the keyframed param at time t, converts the input frame
 * from float wb_px to uint8_t RGBA, calls the wb_vfx function, converts
 * back to float, and returns the frame.
 *
 * Pure C11, no third party.
 */

#include "wbus/wbus_compositor.h"
#include "wbus/wbus_vfx.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Shared pull state: which vfx function to call + param name          */
/* ------------------------------------------------------------------ */
typedef enum {
    VFX_DEEP_FRY = 0,
    VFX_VHS,
    VFX_RGB_GLITCH,
    VFX_POSTERIZE,
    VFX_VIGNETTE,
    VFX_CHROMATIC,
    VFX_CAMERA_SHAKE
} vfx_op;

typedef struct {
    vfx_op op;
    char   param_name[32];  /* keyframable param name */
    float  default_val;     /* fallback if no param bound */
} vfx_node_t;

/* ------------------------------------------------------------------ */
/* Float <-> uint8 conversion helpers                                  */
/* ------------------------------------------------------------------ */
static uint8_t *frame_to_u8(const wb_frame *f) {
    if (!f || !f->px) return NULL;
    int n = f->w * f->h;
    uint8_t *buf = malloc((size_t)n * 4);
    if (!buf) return NULL;
    for (int i = 0; i < n; i++) {
        buf[i*4+0] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, f->px[i].r)) * 255.0f + 0.5f);
        buf[i*4+1] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, f->px[i].g)) * 255.0f + 0.5f);
        buf[i*4+2] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, f->px[i].b)) * 255.0f + 0.5f);
        buf[i*4+3] = (uint8_t)(fminf(1.0f, fmaxf(0.0f, f->px[i].a)) * 255.0f + 0.5f);
    }
    return buf;
}

static void u8_to_frame(const uint8_t *buf, wb_frame *f) {
    if (!buf || !f || !f->px) return;
    int n = f->w * f->h;
    for (int i = 0; i < n; i++) {
        f->px[i].r = buf[i*4+0] / 255.0f;
        f->px[i].g = buf[i*4+1] / 255.0f;
        f->px[i].b = buf[i*4+2] / 255.0f;
        f->px[i].a = buf[i*4+3] / 255.0f;
    }
}

/* ------------------------------------------------------------------ */
/* Shared pull: dispatches to the correct wb_vfx function              */
/* ------------------------------------------------------------------ */
static wb_frame *vfx_pull(wb_node *self, double t,
                          int rx, int ry, int rw, int rh, int phase) {
    (void)rx; (void)ry; (void)rw; (void)rh;
    if (!self->inputs || !self->inputs[0]) return NULL;

    /* G3: request inputs in phase 0 */
    if (phase == 0) {
        wb_node_pull_request(self->inputs[0], t, -1, -1, -1, -1);
        return NULL;
    }

    wb_frame *in = wb_node_pull(self->inputs[0], t, -1, -1, -1, -1);
    if (!in) return NULL;

    vfx_node_t *d = self->user;
    int W = in->w, H = in->h;

    /* Convert float frame to uint8 RGBA */
    uint8_t *buf = frame_to_u8(in);
    if (!buf) return in;

    /* Read keyframed param value at time t */
    float val = wb_node_param_value(self, d->param_name, t);
    if (val == 0.0f) val = d->default_val;

    switch (d->op) {
    case VFX_DEEP_FRY:
        wb_effect_deep_fry(buf, W, H, val);
        break;
    case VFX_VHS:
        wb_effect_vhs(buf, W, H, (float)t, val);
        break;
    case VFX_RGB_GLITCH:
        wb_effect_rgb_glitch(buf, W, H, val);
        break;
    case VFX_POSTERIZE:
        wb_effect_posterize(buf, W * H, (int)val);
        break;
    case VFX_VIGNETTE:
        wb_effect_vignette(buf, W, H, val);
        break;
    case VFX_CHROMATIC:
        wb_chromatic_aberration(buf, W, H, val);
        break;
    case VFX_CAMERA_SHAKE: {
        /* Camera shake: apply a horizontal/vertical offset to the frame.
         * wb_camera_shake is opaque (defined in wb_vfx.c), so we replicate
         * the offset logic locally: LCG random * intensity. */
        unsigned seed = (unsigned)(t * 1000.0 + 12345.0) & 0x7FFFFFFF;
        float rx = ((seed * 1103515245u + 12345u) & 0x7FFFFFFF) / (float)0x7FFFFFFF - 0.5f;
        float ry = ((seed * 22695477u + 1u) & 0x7FFFFFFF) / (float)0x7FFFFFFF - 0.5f;
        int shift_x = (int)(rx * 2.0f * val);
        int shift_y = (int)(ry * 2.0f * val);
        if (shift_x != 0 || shift_y != 0) {
            uint8_t *tmp = malloc((size_t)W * H * 4);
            if (tmp) {
                memcpy(tmp, buf, (size_t)W * H * 4);
                for (int y = 0; y < H; y++) {
                    int sy = y + shift_y;
                    if (sy < 0 || sy >= H) continue;
                    for (int x = 0; x < W; x++) {
                        int sx = x + shift_x;
                        if (sx < 0 || sx >= W) continue;
                        int didx = (y * W + x) * 4;
                        int sidx = (sy * W + sx) * 4;
                        buf[didx+0] = tmp[sidx+0];
                        buf[didx+1] = tmp[sidx+1];
                        buf[didx+2] = tmp[sidx+2];
                        buf[didx+3] = tmp[sidx+3];
                    }
                }
                free(tmp);
            }
        }
        break;
    }
    }

    /* Convert back to float frame */
    u8_to_frame(buf, in);
    free(buf);

    in->roi_x = 0; in->roi_y = 0; in->roi_w = W; in->roi_h = H;
    return in;
}

static void vfx_free(wb_node *n) {
    free(n->user);
}

/* ------------------------------------------------------------------ */
/* Factory: create a vfx effect node                                   */
/* ------------------------------------------------------------------ */
static wb_node *vfx_create(vfx_op op, const char *id,
                           const char *param_name, float default_val) {
    wb_node *n = wb_node_create(WB_NODE_EFFECT, id);
    if (!n) return NULL;
    vfx_node_t *d = calloc(1, sizeof(*d));
    if (!d) { wb_node_destroy(n); return NULL; }
    d->op = op;
    snprintf(d->param_name, sizeof(d->param_name), "%s", param_name);
    d->default_val = default_val;
    n->user = d;
    n->pull = vfx_pull;
    n->free = vfx_free;
    n->n_inputs = 1;
    n->inputs = calloc(1, sizeof(wb_node *));
    return n;
}

/* ------------------------------------------------------------------ */
/* Public factories                                                    */
/* ------------------------------------------------------------------ */

/* Deep fry: extreme contrast + saturation. Param "intensity" (0..n). */
wb_node *wb_node_effect_deep_fry(void) {
    return vfx_create(VFX_DEEP_FRY, "vfx_deep_fry", "intensity", 1.0f);
}

/* VHS: tracking lines + noise + color bleed. Param "intensity" (0..1). */
wb_node *wb_node_effect_vhs(void) {
    return vfx_create(VFX_VHS, "vfx_vhs", "intensity", 0.5f);
}

/* RGB glitch: channel separation. Param "intensity" (0..1). */
wb_node *wb_node_effect_rgb_glitch(void) {
    return vfx_create(VFX_RGB_GLITCH, "vfx_rgb_glitch", "intensity", 0.5f);
}

/* Posterize: reduce color depth. Param "levels" (2..255). */
wb_node *wb_node_effect_posterize(void) {
    return vfx_create(VFX_POSTERIZE, "vfx_posterize", "levels", 8.0f);
}

/* Vignette: darken edges. Param "strength" (0..1). */
wb_node *wb_node_effect_vignette(void) {
    return vfx_create(VFX_VIGNETTE, "vfx_vignette", "strength", 0.5f);
}

/* Chromatic aberration: R/B channel offset. Param "amount" (px). */
wb_node *wb_node_effect_vfx_chromatic(void) {
    return vfx_create(VFX_CHROMATIC, "vfx_chromatic", "amount", 2.0f);
}

/* Camera shake: random frame offset. Param "amount" (px). */
wb_node *wb_node_effect_camera_shake(void) {
    return vfx_create(VFX_CAMERA_SHAKE, "vfx_camera_shake", "amount", 10.0f);
}