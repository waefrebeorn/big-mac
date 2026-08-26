/* wbus_mesh.h — primitive mesh library + Wavefront .obj loader for the
 * R052 software rasterizer (R053).
 *
 * Everything an AGI needs to build 3D diagrams/animations without hand-
 * writing vertex arrays: boxes, spheres, cylinders, cones, toruses, planes,
 * arrows (diagram connectors), plus .obj import for external assets.
 *
 * Opaque mesh handle; vertices + flat-shaded tris extracted straight into
 * wb_rast's scene format. C11, stdlib only, no god header.
 */
#ifndef WUBUS_WBUS_MESH_H
#define WUBUS_WBUS_MESH_H

#include <stdint.h>
#include <stddef.h>
#include "wbus_rast.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_mesh wb_mesh;

wb_mesh *wb_mesh_create(void);
void     wb_mesh_free(wb_mesh *m);

/* Extract into wb_rast scene format (arrays are caller-allocated; pass
 * NULL capacity pointers to just query sizes). Returns 0 on success. */
/* R055b: build a mesh from raw arrays (copies both). The canonical way
 * for modifier/procedural code to materialize geometry. Returns NULL on
 * alloc failure or bad counts. */
/* Deep copy (R065: perf decks own their mesh copy). */
wb_mesh *wb_mesh_copy(const wb_mesh *m);

wb_mesh *wb_mesh_build(const wb_rast_vertex *verts, int nverts,
                       const wb_rast_tri *tris, int ntris);

/* Direct (read-only) access to the internal arrays — for renderers that
 * bake transforms per-frame without copying. Pointers stay valid until
 * the mesh is mutated/freed. */
const wb_rast_vertex *wb_mesh_vert_src(const wb_mesh *m);
const wb_rast_tri    *wb_mesh_tri_src(const wb_mesh *m);

int  wb_mesh_vert_count(const wb_mesh *m);
int  wb_mesh_tri_count(const wb_mesh *m);
void wb_mesh_emit(const wb_mesh *m, wb_rast_vertex *verts, wb_rast_tri *tris);

/* ---- primitives (all centered at origin, unit-ish scale) --------------- */

/* Axis-aligned box, half-extents hx/hy/hz, one color per face pair. */
wb_mesh *wb_mesh_box(float hx, float hy, float hz,
                     uint8_t r, uint8_t g, uint8_t b);

/* UV sphere: latitude bands x longitude segments, Lambert-ish shading. */
wb_mesh *wb_mesh_sphere(float radius, int lat_bands, int lon_bands,
                        uint8_t r, uint8_t g, uint8_t b);

/* Cylinder along Y: radius, height, radial segments. Caps included. */
wb_mesh *wb_mesh_cylinder(float radius, float height, int segs,
                          uint8_t r, uint8_t g, uint8_t b);

/* Cone along Y: base radius, height, radial segments. */
wb_mesh *wb_mesh_cone(float radius, float height, int segs,
                      uint8_t r, uint8_t g, uint8_t b);

/* Torus: major/minor radius, major/minor segments. */
wb_mesh *wb_mesh_torus(float R, float r, int maj, int min,
                       uint8_t r8, uint8_t g8, uint8_t b8);

/* Flat ground plane (2 triangles), half-extent. */
wb_mesh *wb_mesh_plane(float half, uint8_t r, uint8_t g, uint8_t b);

/* Diagram arrow along +Z from z=0 to z=len: shaft cylinder + head cone.
 * The classic "node A points to node B" connector. */
wb_mesh *wb_mesh_arrow(float shaft_r, float head_r, float len,
                       uint8_t r, uint8_t g, uint8_t b);

/* ---- transforms (bake into vertices) ----------------------------------- */

void wb_mesh_translate(wb_mesh *m, float dx, float dy, float dz);
void wb_mesh_scale(wb_mesh *m, float sx, float sy, float sz);
void wb_mesh_rotate_y(wb_mesh *m, float radians);
void wb_mesh_paint(wb_mesh *m, uint8_t r, uint8_t g, uint8_t b);
/* G-SF013: per-face color + gradient paint. */
void wb_mesh_paint_face(wb_mesh *m, int tri, uint8_t r, uint8_t g, uint8_t b);
void wb_mesh_paint_gradient(wb_mesh *m, uint8_t r0, uint8_t g0, uint8_t b0,
                            uint8_t r1, uint8_t g1, uint8_t b1);

/* Merge src into dst (dst keeps its identity). src is untouched so a
 * template primitive can be stamped many times. */
int  wb_mesh_append(wb_mesh *dst, const wb_mesh *src);

/* ---- .obj import (v / f lines, triangulated on load) ------------------- */

wb_mesh *wb_mesh_load_obj(const char *path);

/* R056: export a mesh as Wavefront OBJ (+ MTL with per-face Kd colors so
 * round-trips keep flat shading). Returns 0 on success. */
int wb_mesh_write_obj(const wb_mesh *m, const char *obj_path);


/* R074 hop 118 (G-SF011): capsule + wedge primitives. */
wb_mesh *wb_mesh_capsule(float radius, float height, int segs,
                         uint8_t r, uint8_t g, uint8_t b);
wb_mesh *wb_mesh_wedge(float hx, float hy, float hz,
                       uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_MESH_H */
