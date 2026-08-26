/* wbus_csg.h — R074 hop 188 (G-SF012): constructive solid geometry.
 * Scoped real CSG: subtract an axis-aligned box from any mesh by
 * clipping triangles against the box's six planes (Sutherland-Hodgman
 * per triangle); interior pieces are discarded, boundary-crossing
 * triangles keep their outside fragments. */
#ifndef WUBUS_CSG_H
#define WUBUS_CSG_H

#include "wbus/wbus_mesh.h"

/* out receives the fragments of `m` lying OUTSIDE the AABB
 * [minx,maxx]x[miny,maxy]x[minz,maxz]. Returns 0 on success. */
int wb_mesh_subtract_box(const wb_mesh *m,
                         float minx, float miny, float minz,
                         float maxx, float maxy, float maxz,
                         wb_mesh *out);

#endif /* WUBUS_CSG_H */
