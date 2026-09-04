/* wb_matte_fx.c — Matte Painting + Cutout System (R108).
 *
 * Advanced compositing techniques for YTP/YTPMV:
 *
 * 1. ROTOSCOPING — Separate foreground from background without green screen
 *    - Background subtraction (compare to reference frame)
 *    - Edge-aware refinement (feather, erode, dilate)
 *    - Spill suppression (remove green/blue spill on edges)
 *
 * 2. DEPTH MATTES — Create depth-based alpha masks
 *    - Luma-based depth (brighter = closer)
 *    - Edge-based depth (sharp edges = closer)
 *    - Gradient depth (top = far, bottom = near)
 *
 * 3. 3D MATTE PAINTING — Composite 2.5D scenes with parallax
 *    - Split image into depth layers
 *    - Apply perspective transform per layer
 *    - Camera move creates parallax effect
 *
 * 4. TRACK MATTES — Use one layer to mask another
 *    - Alpha matte (use layer B's alpha to mask layer A)
 *    - Luma matte (use layer B's brightness to mask layer A)
 *    - Inverted variants
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ================================================================
 * ROTOSCOPE ENGINE
 * ================================================================
 *
 * Creates an alpha matte by comparing frames to a background reference.
 * This is how you "cut out" a character without a green screen.
 */

typedef struct {
    uint8_t *background;   /* reference background frame */
    int width, height;
    int threshold;         /* pixel difference threshold (0-255) */
    int feather;           /* feather radius in pixels */
    int erode;             /* erode size (shrink matte) */
    int dilate;            /* dilate size (grow matte) */
    float spill_suppress;  /* spill suppression strength (0-1) */
} wb_rotoscope;

void wb_rotoscope_init(wb_rotoscope *r, int w, int h) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->width = w;
    r->height = h;
    r->threshold = 30;
    r->feather = 2;
    r->erode = 1;
    r->dilate = 2;
    r->spill_suppress = 0.5f;
    r->background = (uint8_t *)calloc(w * h * 4, 1);
}

/* Create alpha matte from foreground + background */
void wb_rotoscope_apply(wb_rotoscope *r, uint8_t *foreground,
                          uint8_t *alpha_out) {
    if (!r || !foreground || !alpha_out || !r->background) return;
    
    int w = r->width, h = r->height;
    
    /* Step 1: Difference matte */
    for (int i = 0; i < w * h; i++) {
        int dr = abs((int)foreground[i*4]   - (int)r->background[i*4]);
        int dg = abs((int)foreground[i*4+1] - (int)r->background[i*4+1]);
        int db = abs((int)foreground[i*4+2] - (int)r->background[i*4+2]);
        int diff = (dr + dg + db) / 3;
        
        /* Above threshold = foreground (white), below = background (black) */
        alpha_out[i*4+3] = (diff > r->threshold) ? 255 : 0;
        alpha_out[i*4] = alpha_out[i*4+1] = alpha_out[i*4+2] = alpha_out[i*4+3];
    }
    
    /* Step 2: Erode (shrink to remove edge noise) */
    if (r->erode > 0) {
        uint8_t *tmp = (uint8_t *)malloc(w * h * 4);
        if (!tmp) return;
        memcpy(tmp, alpha_out, w * h * 4);
        
        for (int y = r->erode; y < h - r->erode; y++) {
            for (int x = r->erode; x < w - r->erode; x++) {
                int min_a = 255;
                for (int dy = -r->erode; dy <= r->erode; dy++) {
                    for (int dx = -r->erode; dx <= r->erode; dx++) {
                        int a = tmp[((y+dy)*w + (x+dx))*4 + 3];
                        if (a < min_a) min_a = a;
                    }
                }
                alpha_out[(y*w + x)*4 + 3] = min_a;
            }
        }
        free(tmp);
    }
    
    /* Step 3: Dilate (grow to fill gaps) */
    if (r->dilate > 0) {
        uint8_t *tmp = (uint8_t *)malloc(w * h * 4);
        if (!tmp) return;
        memcpy(tmp, alpha_out, w * h * 4);
        
        for (int y = r->dilate; y < h - r->dilate; y++) {
            for (int x = r->dilate; x < w - r->dilate; x++) {
                int max_a = 0;
                for (int dy = -r->dilate; dy <= r->dilate; dy++) {
                    for (int dx = -r->dilate; dx <= r->dilate; dx++) {
                        int a = tmp[((y+dy)*w + (x+dx))*4 + 3];
                        if (a > max_a) max_a = a;
                    }
                }
                alpha_out[(y*w + x)*4 + 3] = max_a;
            }
        }
        free(tmp);
    }
    
    /* Step 4: Feather (soften edges) */
    if (r->feather > 0) {
        uint8_t *tmp = (uint8_t *)malloc(w * h * 4);
        if (!tmp) return;
        memcpy(tmp, alpha_out, w * h * 4);
        
        int radius = r->feather;
        for (int y = radius; y < h - radius; y++) {
            for (int x = radius; x < w - radius; x++) {
                int sum = 0, count = 0;
                for (int dy = -radius; dy <= radius; dy++) {
                    for (int dx = -radius; dx <= radius; dx++) {
                        float dist = sqrtf(dx*dx + dy*dy);
                        if (dist <= radius) {
                            sum += tmp[((y+dy)*w + (x+dx))*4 + 3];
                            count++;
                        }
                    }
                }
                alpha_out[(y*w + x)*4 + 3] = (uint8_t)(sum / count);
            }
        }
        free(tmp);
    }
    
    /* Step 5: Spill suppression (remove color cast on edges) */
    if (r->spill_suppress > 0) {
        for (int i = 0; i < w * h; i++) {
            float a = alpha_out[i*4+3] / 255.0f;
            if (a > 0.1f && a < 0.9f) {
                /* Edge pixel - suppress spill */
                float spill = r->spill_suppress;
                uint8_t r_val = foreground[i*4];
                uint8_t g_val = foreground[i*4+1];
                uint8_t b_val = foreground[i*4+2];
                
                /* Detect green spill */
                if (g_val > r_val && g_val > b_val) {
                    uint8_t avg = (r_val + b_val) / 2;
                    foreground[i*4+1] = (uint8_t)(g_val * (1-spill) + avg * spill);
                }
                /* Detect blue spill */
                if (b_val > r_val && b_val > g_val) {
                    uint8_t avg = (r_val + g_val) / 2;
                    foreground[i*4+2] = (uint8_t)(b_val * (1-spill) + avg * spill);
                }
            }
        }
    }
}

void wb_rotoscope_free(wb_rotoscope *r) {
    if (!r) return;
    free(r->background);
}

/* ================================================================
 * DEPTH MATTE
 * ================================================================
 *
 * Creates a depth-based alpha mask from various inputs.
 */

typedef enum {
    DEPTH_LUMA = 0,        /* brightness = depth */
    DEPTH_EDGE,            /* edge sharpness = depth */
    DEPTH_GRADIENT,        /* vertical gradient = depth */
    DEPTH_RADIAL           /* distance from center = depth */
} wb_depth_type;

typedef struct {
    int width, height;
    int depth_type;
    float near_plane;      /* closest depth (white) */
    float far_plane;       /* farthest depth (black) */
    int blur_radius;       /* blur the depth map for smoothness */
} wb_depth_matte;

void wb_depth_init(wb_depth_matte *d, int w, int h) {
    if (!d) return;
    memset(d, 0, sizeof(*d));
    d->width = w;
    d->height = h;
    d->depth_type = DEPTH_LUMA;
    d->near_plane = 1.0f;
    d->far_plane = 0.0f;
    d->blur_radius = 3;
}

/* Generate depth matte from frame */
void wb_depth_generate(wb_depth_matte *d, const uint8_t *frame,
                        uint8_t *depth_out) {
    if (!d || !frame || !depth_out) return;
    
    int w = d->width, h = d->height;
    
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int off = (y*w + x) * 4;
            float depth = 0.5f;
            
            switch (d->depth_type) {
                case DEPTH_LUMA: {
                    /* Brighter = closer */
                    depth = (frame[off] + frame[off+1] + frame[off+2]) / (3.0f * 255.0f);
                    break;
                }
                case DEPTH_GRADIENT: {
                    /* Top = far, bottom = near */
                    depth = (float)y / h;
                    break;
                }
                case DEPTH_RADIAL: {
                    /* Center = near, edges = far */
                    float dx = (x - w/2.0f) / (w/2.0f);
                    float dy = (y - h/2.0f) / (h/2.0f);
                    float dist = sqrtf(dx*dx + dy*dy);
                    depth = 1.0f - fminf(dist, 1.0f);
                    break;
                }
                case DEPTH_EDGE: {
                    /* Simple edge detection = depth */
                    if (x > 0 && x < w-1 && y > 0 && y < h-1) {
                        int gx = abs((int)frame[off+4] - (int)frame[off-4]);
                        int gy = abs((int)frame[off+w*4] - (int)frame[off-w*4]);
                        depth = fminf((gx + gy) / 510.0f, 1.0f);
                    }
                    break;
                }
            }
            
            /* Map to near/far range */
            depth = d->far_plane + (d->near_plane - d->far_plane) * depth;
            uint8_t val = (uint8_t)(depth * 255);
            depth_out[off] = depth_out[off+1] = depth_out[off+2] = val;
            depth_out[off+3] = 255;
        }
    }
    
    /* Blur for smoothness */
    if (d->blur_radius > 0) {
        uint8_t *tmp = (uint8_t *)malloc(w * h * 4);
        if (!tmp) return;
        memcpy(tmp, depth_out, w * h * 4);
        
        int r = d->blur_radius;
        for (int y = r; y < h - r; y++) {
            for (int x = r; x < w - r; x++) {
                int sum = 0, count = 0;
                for (int dy = -r; dy <= r; dy++) {
                    for (int dx = -r; dx <= r; dx++) {
                        sum += tmp[((y+dy)*w + (x+dx))*4];
                        count++;
                    }
                }
                depth_out[(y*w + x)*4] = depth_out[(y*w + x)*4+1] = 
                depth_out[(y*w + x)*4+2] = (uint8_t)(sum / count);
            }
        }
        free(tmp);
    }
}

/* ================================================================
 * 3D PARALLAX (2.5D Matte Painting)
 * ================================================================
 *
 * Splits an image into depth layers and applies perspective transforms
 * to create a 2.5D effect with camera parallax.
 */

typedef struct {
    int width, height;
    int n_layers;
    float camera_x, camera_y, camera_z;
    float camera_rot_x, camera_rot_y;
    float fov;
} wb_parallax_3d;

void wb_parallax_init(wb_parallax_3d *p, int w, int h) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->width = w;
    p->height = h;
    p->n_layers = 3; /* near, mid, far */
    p->camera_z = 5.0f;
    p->fov = 60.0f * M_PI / 180.0f;
}

/* Apply parallax to a layer based on its depth */
void wb_parallax_apply_layer(const wb_parallax_3d *p, const uint8_t *src,
                               uint8_t *dst, float layer_depth) {
    if (!p || !src || !dst) return;
    
    int w = p->width, h = p->height;
    
    /* Calculate parallax offset based on depth and camera */
    float depth_factor = 1.0f / (1.0f + layer_depth * 2.0f);
    float offset_x = p->camera_x * depth_factor * 50.0f;
    float offset_y = p->camera_y * depth_factor * 50.0f;
    float scale = 1.0f + (p->camera_z - 5.0f) * 0.1f * depth_factor;
    
    /* Clear output */
    memset(dst, 0, w * h * 4);
    
    /* Apply transform */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* Inverse transform to find source pixel */
            float sx = (x - w/2.0f) / scale + w/2.0f - offset_x;
            float sy = (y - h/2.0f) / scale + h/2.0f - offset_y;
            
            int src_x = (int)(sx + 0.5f);
            int src_y = (int)(sy + 0.5f);
            
            if (src_x >= 0 && src_x < w && src_y >= 0 && src_y < h) {
                int dst_off = (y*w + x) * 4;
                int src_off = (src_y*w + src_x) * 4;
                dst[dst_off] = src[src_off];
                dst[dst_off+1] = src[src_off+1];
                dst[dst_off+2] = src[src_off+2];
                dst[dst_off+3] = src[src_off+3];
            }
        }
    }
}
