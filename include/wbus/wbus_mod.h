/* wbus_mod.h — non-destructive modifier stack for meshes (R055b).
 *
 * Blender's core modeling idea, miniaturized: modifiers are queued on a
 * mesh and evaluated in order to produce derived geometry. The source
 * mesh is never touched — "apply" means evaluate the whole stack into a
 * new mesh.
 *
 * Modifiers implemented (v1):
 *   ARRAY    — n copies spaced along an axis (diagram rows/columns)
 *   MIRROR   — mirror across X/Y/Z plane
 *   WAVE     — sinusoidal Y displacement along X (flags/water)
 *   TWIST    — rotate vertices progressively along Y (spirals)
 *   SOLIDIFY — shell a plane/strip by extruding a mirrored copy + rim
 *
 * C11, opaque, self-contained.
 */
#ifndef WUBUS_WBUS_MOD_H
#define WUBUS_WBUS_MOD_H

#include <stdint.h>
#include <stddef.h>
#include "wbus_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_modstack wb_modstack;

wb_modstack *wb_mod_create(void);
void         wb_mod_free(wb_modstack *ms);

typedef enum {
    WB_MOD_ARRAY = 1,
    WB_MOD_MIRROR,
    WB_MOD_WAVE,
    WB_MOD_TWIST,
    WB_MOD_SOLIDIFY
} wb_mod_type;

/* Adders return modifier index or -1. Params follow Blender semantics,
 * simplified: */
int wb_mod_add_array(wb_modstack *ms, int count,
                     float dx, float dy, float dz);
int wb_mod_add_mirror(wb_modstack *ms, int axis);      /* 0=x 1=y 2=z */
int wb_mod_add_wave(wb_modstack *ms, float amp, float waves);
int wb_mod_add_twist(wb_modstack *ms, float radians_total);
int wb_mod_add_solidify(wb_modstack *ms, float thickness);

/* Evaluate src + full stack into a fresh mesh. Returns NULL on error.
 * The result is owned by the caller. */
wb_mesh *wb_mod_apply(const wb_mesh *src, const wb_modstack *ms);

int wb_mod_count(const wb_modstack *ms);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_MOD_H */
