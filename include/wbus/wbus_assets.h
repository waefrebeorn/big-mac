/* wbus_assets.h — asset library index over Kenney-style kit directories
 * (R057).
 *
 * Layout on disk (the "library"):
 *   <root>/<kit>/<model>.glb        e.g. assets/kits/car-kit/sedan.glb
 *
 * The module scans the root once, indexes every kit/model, and serves:
 *   - kit list / model list
 *   - load-by-name with a small cache (meshes are expensive to parse)
 *   - a default root (./assets/kits) overridable by env WB_ASSETS_ROOT
 *
 * C11, opaque, self-contained.
 */
#ifndef WUBUS_WBUS_ASSETS_H
#define WUBUS_WBUS_ASSETS_H

#include <stdint.h>
#include <stddef.h>
#include "wbus_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_assets wb_assets;

/* Open/scan a library root. Returns NULL if the dir doesn't exist. */
wb_assets *wb_assets_open(const char *root);
void       wb_assets_close(wb_assets *a);

/* Default-root convenience: uses WB_ASSETS_ROOT env or "./assets/kits". */
wb_assets *wb_assets_open_default(void);

int  wb_assets_kit_count(const wb_assets *a);
/* Kit name by index (owned by the assets struct; valid until close). */
const char *wb_assets_kit_name(const wb_assets *a, int kit);
int  wb_assets_model_count(const wb_assets *a, int kit);
const char *wb_assets_model_name(const wb_assets *a, int kit, int model);

/* Load a model (parses GLB through wb_gltf; cached). Returns a mesh the
 * CALLER must not free (cache-owned). Returns NULL on miss/parse error. */
wb_mesh *wb_assets_load(wb_assets *a, const char *kit, const char *model);

/* Total indexed models across all kits. */
int  wb_assets_total(const wb_assets *a);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_ASSETS_H */
