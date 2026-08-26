/* wbus_scenedesc.h — R074 hop 171 (G-SF079): text scene descriptions.
 * Replaces the hardcoded --starfox scene with a loadable format:
 *   box <w> <h> <d> <r> <g> <b>
 *   cone <radius> <height> <sides> <r> <g> <b>
 *   key <obj> <t> <x> <y> <z> <scale>
 *   shake <amt>
 *   fog <near> <far>
 * Comments (#) and blank lines ignored. */
#ifndef WUBUS_SCENEDESC_H
#define WUBUS_SCENEDESC_H

#include "wbus/wbus_anim.h"

/* Load a scene description into an existing (fresh) wb_anim.
 * Returns 0 on success, line number of the first error (negative) otherwise. */
int wb_scenedesc_load(wb_anim *a, const char *path);

#endif
