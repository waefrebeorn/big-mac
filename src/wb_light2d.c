/* wb_light2d.c — 2D lighting, shadows, and camera effects (R077 Phase 3).
 *
 * Normal-mapped 2D lighting, shadow casting, parallax scrolling,
 * Ken Burns effect, and 2.5D perspective projection.
 *
 * Works with wb_rast.c for 3D and wb_compositor.c for compositing.
 * Pure C11, no third party.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ===================================================================
 * 2D Light Types
 * =================================================================== */

#define WB_2D_MAX_LIGHTS 16

typedef enum {
    WB_LIGHT_POINT = 0,
    WB_LIGHT_DIRECTIONAL,
    WB_LIGHT_SPOT,
    WB_LIGHT_AMBIENT
} wb_light_type;

typedef struct {
    wb_light_type type;
    float x, y;          /* position (point/spot) */
    float dir_x, dir_y;  /* direction (directional/spot), normalized */
    float r, g, b;       /* color (0..1) */
    float intensity;     /* brightness */
    float radius;        /* falloff radius (point/spot) */
    float angle;         /* cone angle (spot, radians) */
    float softness;      /* edge softness (spot, 0..1) */
} wb_light2d;

typedef struct {
    wb_light2d lights[WB_2D_MAX_LIGHTS];
    int count;
    float ambient_r, ambient_g, ambient_b;  /* global ambient */
} wb_light_env;

void wb_light_env_init(wb_light_env *env) {
    memset(env, 0, sizeof(*env));
    env->ambient_r = 0.1f;
    env->ambient_g = 0.1f;
    env->ambient_b = 0.1f;
}

int wb_light_env_add(wb_light_env *env, const wb_light2d *light) {
    if (env->count >= WB_2D_MAX_LIGHTS) return -1;
    env->lights[env->count++] = *light;
    return env->count - 1;
}

/* ===================================================================
 * Normal-Mapped 2D Lighting
 * =================================================================== */

/* Apply lighting to an RGBA sprite using its normal map.
 * normal_map: RGBA where R = normal_x*127+128, G = normal_y*127+128, B = normal_z*127+128
 * output: lit RGBA (modified in place)
 */
void wb_light2d_apply(const wb_light_env *env, uint8_t *output,
                       const uint8_t *normal_map, int width, int height) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;

            /* Decode normal */
            float nx = (normal_map[idx+0] / 255.0f) * 2.0f - 1.0f;
            float ny = (normal_map[idx+1] / 255.0f) * 2.0f - 1.0f;
            float nz = (normal_map[idx+2] / 255.0f) * 2.0f - 1.0f;

            /* Normalize */
            float nlen = sqrtf(nx*nx + ny*ny + nz*nz);
            if (nlen > 0.01f) { nx /= nlen; ny /= nlen; nz /= nlen; }

            /* Accumulate light */
            float lit_r = env->ambient_r;
            float lit_g = env->ambient_g;
            float lit_b = env->ambient_b;

            for (int i = 0; i < env->count; i++) {
                const wb_light2d *l = &env->lights[i];
                float lx, ly, lz;  /* light direction (toward surface) */
                float atten = 1.0f;

                switch (l->type) {
                case WB_LIGHT_AMBIENT:
                    lit_r += l->r * l->intensity;
                    lit_g += l->g * l->intensity;
                    lit_b += l->b * l->intensity;
                    continue;

                case WB_LIGHT_DIRECTIONAL:
                    lx = -l->dir_x;
                    ly = -l->dir_y;
                    lz = 0.7f;  /* slight angle toward viewer */
                    atten = 1.0f;
                    break;

                case WB_LIGHT_POINT:
                case WB_LIGHT_SPOT:
                    lx = l->x - (float)x;
                    ly = l->y - (float)y;
                    float dist = sqrtf(lx*lx + ly*ly);
                    if (dist < 0.01f) dist = 0.01f;
                    lx /= dist;
                    ly /= dist;
                    lz = 1.0f - dist / l->radius;
                    if (lz < 0) lz = 0;
                    /* Quadratic falloff */
                    atten = 1.0f / (1.0f + 0.1f * dist + 0.01f * dist * dist);
                    if (dist > l->radius) atten = 0;

                    /* Spot cone */
                    if (l->type == WB_LIGHT_SPOT) {
                        float spot_dot = lx * l->dir_x + ly * l->dir_y;
                        float cos_angle = cosf(l->angle);
                        if (spot_dot < cos_angle) {
                            atten = 0;
                        } else if (l->softness > 0) {
                            float edge = (spot_dot - cos_angle) / (1.0f - cos_angle + 0.001f);
                            float soft_thresh = 1.0f - l->softness;
                            if (edge < soft_thresh) {
                                atten *= edge / soft_thresh;
                            }
                        }
                    }
                    break;
                }

                /* Diffuse: dot(normal, light_dir) */
                float diff = nx * lx + ny * ly + nz * lz;
                if (diff < 0) diff = 0;

                /* Specular (Blinn-Phong) */
                float hx = lx, hy = ly, hz = lz + 1.0f;  /* half vector (viewer at +z) */
                float hlen = sqrtf(hx*hx + hy*hy + hz*hz);
                if (hlen > 0.01f) { hx /= hlen; hy /= hlen; hz /= hlen; }
                float spec = nx * hx + ny * hy + nz * hz;
                if (spec < 0) spec = 0;
                spec = powf(spec, 32.0f) * 0.5f;

                lit_r += (l->r * diff + spec) * atten * l->intensity;
                lit_g += (l->g * diff + spec) * atten * l->intensity;
                lit_b += (l->b * diff + spec) * atten * l->intensity;
            }

            /* Modulate sprite color by lighting */
            float sprite_r = output[idx+0] / 255.0f;
            float sprite_g = output[idx+1] / 255.0f;
            float sprite_b = output[idx+2] / 255.0f;

            output[idx+0] = (uint8_t)(fminf(1, sprite_r * lit_r) * 255.0f + 0.5f);
            output[idx+1] = (uint8_t)(fminf(1, sprite_g * lit_g) * 255.0f + 0.5f);
            output[idx+2] = (uint8_t)(fminf(1, sprite_b * lit_b) * 255.0f + 0.5f);
        }
    }
}

/* ===================================================================
 * 2D Shadow Casting
 * =================================================================== */

/* Cast shadows from a set of occluder segments onto a light.
 * occluders: array of (x1,y1,x2,y2) segments
 * For each pixel, ray-march toward light, check occlusion.
 */
void wb_light2d_shadow(uint8_t *shadow_map, int width, int height,
                        float light_x, float light_y,
                        const float *occluders, int n_occluders) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;

            /* Ray from pixel toward light */
            float dx = light_x - x;
            float dy = light_y - y;
            float dist = sqrtf(dx*dx + dy*dy);
            if (dist < 1.0f) { shadow_map[idx] = 255; continue; }

            dx /= dist;
            dy /= dist;

            /* March toward light, check segment intersection */
            int occluded = 0;
            float step = 2.0f;
            for (float t = 0; t < dist; t += step) {
                float px = x + dx * t;
                float py = y + dy * t;

                for (int i = 0; i < n_occluders; i++) {
                    float x1 = occluders[i*4+0], y1 = occluders[i*4+1];
                    float x2 = occluders[i*4+2], y2 = occluders[i*4+3];

                    /* Point-to-segment distance */
                    float sx = x2 - x1, sy = y2 - y1;
                    float len2 = sx*sx + sy*sy;
                    if (len2 < 0.01f) continue;

                    float t_proj = ((px - x1) * sx + (py - y1) * sy) / len2;
                    if (t_proj < 0 || t_proj > 1) continue;

                    float closest_x = x1 + t_proj * sx;
                    float closest_y = y1 + t_proj * sy;
                    float ddx = px - closest_x, ddy = py - closest_y;
                    if (ddx*ddx + ddy*ddy < 4.0f) {  /* within 2px */
                        occluded = 1;
                        break;
                    }
                }
                if (occluded) break;
            }

            shadow_map[idx] = occluded ? 60 : 255;  /* 60 = shadow, 255 = lit */
        }
    }
}

/* ===================================================================
 * Parallax Scrolling
 * =================================================================== */

typedef struct {
    uint8_t *layer_rgba;   /* RGBA pixel data */
    int width, height;
    float depth;           /* 0 = far (moves slow), 1 = near (moves fast) */
    float offset_x, offset_y;  /* current scroll offset */
    int wrap;              /* tile horizontally */
} wb_parallax_layer;

typedef struct {
    wb_parallax_layer *layers;
    int n_layers;
    float camera_x, camera_y;
} wb_parallax_view;

/* Update parallax offsets based on camera position */
void wb_parallax_update(wb_parallax_view *view) {
    for (int i = 0; i < view->n_layers; i++) {
        wb_parallax_layer *l = &view->layers[i];
        /* Far layers (low depth) move slowly */
        float factor = l->depth * 0.5f + 0.1f;
        l->offset_x = -view->camera_x * factor;
        l->offset_y = -view->camera_y * factor;
    }
}

/* Render parallax layer to output buffer (with wrapping) */
void wb_parallax_render(const wb_parallax_view *view, int layer_idx,
                         uint8_t *output, int out_w, int out_h) {
    if (layer_idx < 0 || layer_idx >= view->n_layers) return;
    const wb_parallax_layer *l = &view->layers[layer_idx];

    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < out_w; x++) {
            int src_x, src_y;
            if (l->wrap) {
                src_x = ((int)(x + l->offset_x) % l->width + l->width) % l->width;
                src_y = ((int)(y + l->offset_y) % l->height + l->height) % l->height;
            } else {
                src_x = (int)(x + l->offset_x);
                src_y = (int)(y + l->offset_y);
                if (src_x < 0 || src_x >= l->width || src_y < 0 || src_y >= l->height)
                    continue;
            }

            int src_idx = (src_y * l->width + src_x) * 4;
            int dst_idx = (y * out_w + x) * 4;

            /* Alpha composite */
            float alpha = l->layer_rgba[src_idx+3] / 255.0f;
            if (alpha <= 0) continue;
            float inv_a = 1.0f - alpha;

            output[dst_idx+0] = (uint8_t)(l->layer_rgba[src_idx+0] * alpha + output[dst_idx+0] * inv_a);
            output[dst_idx+1] = (uint8_t)(l->layer_rgba[src_idx+1] * alpha + output[dst_idx+1] * inv_a);
            output[dst_idx+2] = (uint8_t)(l->layer_rgba[src_idx+2] * alpha + output[dst_idx+2] * inv_a);
            output[dst_idx+3] = (uint8_t)fminf(255, output[dst_idx+3] + l->layer_rgba[src_idx+3]);
        }
    }
}

/* ===================================================================
 * Ken Burns Effect (pan + zoom on still image)
 * =================================================================== */

typedef struct {
    float start_x, start_y;    /* normalized 0..1 */
    float start_scale;
    float end_x, end_y;
    float end_scale;
    float duration;
    float time;
} wb_ken_burns;

void wb_ken_burns_init(wb_ken_burns *kb) {
    kb->start_x = 0.0f; kb->start_y = 0.0f;
    kb->start_scale = 1.0f;
    kb->end_x = 0.5f; kb->end_y = 0.3f;
    kb->end_scale = 1.5f;
    kb->duration = 5.0f;
    kb->time = 0.0f;
}

void wb_ken_burns_update(wb_ken_burns *kb, float dt) {
    kb->time += dt;
    if (kb->time > kb->duration) kb->time = kb->duration;
}

/* Apply Ken Burns to copy a source image into output.
 * Ease in/out interpolation.
 */
void wb_ken_burns_apply(const wb_ken_burns *kb,
                         const uint8_t *src, int src_w, int src_h,
                         uint8_t *dst, int dst_w, int dst_h) {
    float t = kb->time / kb->duration;
    if (t > 1.0f) t = 1.0f;

    /* Ease in/out */
    float ease = t < 0.5f ? 2.0f*t*t : 1.0f - (-2.0f*t+2.0f)*(-2.0f*t+2.0f)/2.0f;

    float cx = kb->start_x + (kb->end_x - kb->start_x) * ease;
    float cy = kb->start_y + (kb->end_y - kb->start_y) * ease;
    float scale = kb->start_scale + (kb->end_scale - kb->start_scale) * ease;

    /* Source region to sample */
    float sample_w = (float)dst_w / scale;
    float sample_h = (float)dst_h / scale;
    float sample_x = cx * (src_w - sample_w);
    float sample_y = cy * (src_h - sample_h);

    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            /* Bilinear sample from source */
            float sx = sample_x + (float)x / dst_w * sample_w;
            float sy = sample_y + (float)y / dst_h * sample_h;

            int x0 = (int)sx, y0 = (int)sy;
            int x1 = x0 + 1, y1 = y0 + 1;
            if (x0 < 0) x0 = 0; if (x0 >= src_w) x0 = src_w - 1;
            if (x1 < 0) x1 = 0; if (x1 >= src_w) x1 = src_w - 1;
            if (y0 < 0) y0 = 0; if (y0 >= src_h) y0 = src_h - 1;
            if (y1 < 0) y1 = 0; if (y1 >= src_h) y1 = src_h - 1;

            float fx = sx - x0, fy = sy - y0;

            for (int c = 0; c < 4; c++) {
                float v00 = src[(y0*src_w+x0)*4+c];
                float v10 = src[(y0*src_w+x1)*4+c];
                float v01 = src[(y1*src_w+x0)*4+c];
                float v11 = src[(y1*src_w+x1)*4+c];
                float val = v00*(1-fx)*(1-fy) + v10*fx*(1-fy) + v01*(1-fx)*fy + v11*fx*fy;
                dst[(y*dst_w+x)*4+c] = (uint8_t)(val + 0.5f);
            }
        }
    }
}

/* ===================================================================
 * 2.5D Perspective Projection
 * =================================================================== */

/* Project a 3D point to 2D screen with perspective.
 * Camera at (0,0,cam_z) looking down -z.
 */
typedef struct {
    float fov;           /* field of view (radians) */
    float cam_x, cam_y, cam_z;
    float target_x, target_y, target_z;
    float near_plane, far_plane;
} wb_camera3d;

void wb_project_point(const wb_camera3d *cam, float wx, float wy, float wz,
                       float *screen_x, float *screen_y, float *depth,
                       int view_w, int view_h) {
    /* Translate relative to camera */
    float dx = wx - cam->cam_x;
    float dy = wy - cam->cam_y;
    float dz = wz - cam->cam_z;

    /* Simple perspective: divide by distance */
    float dist = -dz;  /* camera looks down -z */
    if (dist < cam->near_plane) dist = cam->near_plane;

    float f = (float)view_h * 0.5f / tanf(cam->fov * 0.5f);

    *screen_x = (float)view_w * 0.5f + dx * f / dist;
    *screen_y = (float)view_h * 0.5f - dy * f / dist;
    *depth = dist;
}

/* Skew/Shear transform for 2D */
void wb_transform_skew(uint8_t *dst, const uint8_t *src,
                        int w, int h, float skew_x, float skew_y) {
    memset(dst, 0, w * h * 4);
    float cx = w * 0.5f, cy = h * 0.5f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* Inverse transform: where does this dst pixel come from? */
            float dx = x - cx, dy = y - cy;
            float src_x = dx - skew_x * dy + cx;
            float src_y = dy - skew_y * dx + cy;

            int sx = (int)src_x, sy = (int)src_y;
            if (sx < 0 || sx >= w || sy < 0 || sy >= h) continue;

            int di = (y * w + x) * 4;
            int si = (sy * w + sx) * 4;
            dst[di+0] = src[si+0];
            dst[di+1] = src[si+1];
            dst[di+2] = src[si+2];
            dst[di+3] = src[si+3];
        }
    }
}
