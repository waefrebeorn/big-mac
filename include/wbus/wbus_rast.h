/* wbus_rast.h — Jet-style fixed-function software triangle rasterizer (R052).
 *
 * Doctrine: SLERM of the techniques proven by CubeCoders/Jet (C++17,
 * AGPL) into pure C11 — integer fixed-point edge functions, flat-shaded
 * RGB565 output, zero allocations on the hot path, no hidden globals.
 * This module knows nothing about SDL, ffmpeg or the DAW: it renders
 * triangles into an RGBA byte buffer the caller owns.
 *
 * Pipeline contract (matches wb_video_export / wb_cgi consumers):
 *   1. caller fills a vertex list (model space) + index list
 *   2. wb_rast_render(): transform -> project -> rasterize to RGBA
 *   3. caller composites the buffer over video frames (ffmpeg overlay)
 *
 * Opaque struct, C11, stdlib only. No god header. Self-contained.
 */
#ifndef WUBUS_WBUS_RAST_H
#define WUBUS_WBUS_RAST_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_rast_ctx wb_rast_ctx;

/* Create a rasterizer targeting a w x h RGBA8 framebuffer.
 * Returns NULL on alloc failure. One context = one target size. */
wb_rast_ctx *wb_rast_create(int w, int h);
void         wb_rast_destroy(wb_rast_ctx *r);

/* ---- scene description (caller-owned arrays, copied in) ---------------- */

typedef struct {
    float x, y, z;        /* model space */
} wb_rast_vertex;

typedef struct {
    int   v0, v1, v2;     /* indices into the vertex array */
    uint8_t r, g, b;      /* flat-shade color for this triangle */
} wb_rast_tri;

/* Set the frame's geometry. Copies both arrays (caller may free after).
 * Camera: orbit angles (radians), distance, and vertical look offset. */
int  wb_rast_set_scene(wb_rast_ctx *r,
                       const wb_rast_vertex *verts, int nverts,
                       const wb_rast_tri *tris, int ntris);
void wb_rast_set_camera(wb_rast_ctx *r,
                        float rx, float ry, float rz,
                        float dist, float y_off);

/* ---- render ------------------------------------------------------------ */

/* Render the current scene. Output is written into out_rgba
 * (w*h*4 bytes, caller-allocated). Pixels are straight alpha=255 where
 * drawn, 0 where untouched — so the compositor can key on alpha. */
void wb_rast_render(wb_rast_ctx *r, uint8_t *out_rgba);

/* Back-face culling toggle (default on). */
void wb_rast_set_cull(wb_rast_ctx *r, int on);

/* ---- R055: lights & depth (MiniBlender shading) ------------------------ */

/* Directional light direction (normalized internally) + intensity 0..1.
 * Set dir to 0,0,0 to disable lighting (raw flat colors). */
void wb_rast_set_sun(wb_rast_ctx *r, float dx, float dy, float dz,
                     float intensity);

/* Specular highlight strength (0 = off). View is fixed at -z. */
void wb_rast_set_specular(wb_rast_ctx *r, float strength);

/* Enable a depth buffer for correct interpenetration (disables the
 * painter's sort path). Default: off (painter). */
void wb_rast_set_zbuffer(wb_rast_ctx *r, int on);
/* R074 hop 120 (G-SF007): camera focal length (FOV control). */
void wb_rast_set_focal(wb_rast_ctx *r, float focal);
/* G-SF039: light back faces with flipped normal. */
void wb_rast_set_two_sided(wb_rast_ctx *r, int on);
/* R074 hop 118 (G-SF034): wireframe/debug draw. */
void wb_rast_set_wireframe(wb_rast_ctx *r, int on);
/* G-SF027: 0=flat shading (default), 1=gouraud per-vertex lighting. */
void wb_rast_set_shading(wb_rast_ctx *r, int gouraud);
/* G-SF035: viewport scissor — clip rasterization to a sub-rect. */
void wb_rast_set_scissor(wb_rast_ctx *r, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_RAST_H */
