/* wbus_gltf.h — GLB (glTF 2.0) mesh import for the asset pipeline (R056).
 *
 * Minimal, dependency-free GLB parser: container header + JSON chunk scan
 * + accessor extraction. Supports POSITION (VEC3 float) and indices
 * (u32/u16) with node-hierarchy transform flattening; optional COLOR_0
 * becomes per-tri color. Skins/animations/textures are out of scope.
 *
 * C11, opaque, self-contained. See docs/R056-asset-io-scope.md.
 */
#ifndef WUBUS_WBUS_GLTF_H
#define WUBUS_WBUS_GLTF_H

#include <stdint.h>
#include <stddef.h>
#include "wbus_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the first mesh of a .glb file, flattened through the node tree.
 * Returns NULL on parse failure / unsupported features. */
wb_mesh *wb_gltf_load_glb(const char *path);

/* Same but applies a uniform scale + base color fallback when the file
 * has no COLOR_0 (Kenney models are untextured geometry). */
wb_mesh *wb_gltf_load_glb_ex(const char *path, float scale,
                             uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_GLTF_H */
