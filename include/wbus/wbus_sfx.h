/* wbus_sfx.h — R074 hop 150 (G-SF070): procedural game-SFX synthesis.
 * Deterministic (fixed seeds, no fast-math) per the G-SF097 policy.
 */
#ifndef WUBUS_SFX_H
#define WUBUS_SFX_H

#include <stdint.h>

/* All renderers write at most n floats and return the number of
 * non-tail samples written (-1 on bad args). Buffers are zero-filled
 * past the end. */

int wb_sfx_laser(float *out, int n, uint32_t sr,
                 float f0, float f1, float dur);
int wb_sfx_explosion(float *out, int n, uint32_t sr, float dur);
int wb_sfx_pickup(float *out, int n, uint32_t sr);
int wb_sfx_powerup(float *out, int n, uint32_t sr, float dur);

#endif /* WUBUS_SFX_H */
