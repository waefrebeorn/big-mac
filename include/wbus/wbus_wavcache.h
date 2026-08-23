/* wbus_wavcache.h — waveform LOD pyramid cache (R066).
 *
 * The GUI perf fix: drawing waveforms by scanning raw samples every frame
 * dies on long clips. Instead, compute a MIN/MAX pyramid once per clip
 * (levels of ~256x reduction each) and draw from the level closest to the
 * zoom — O(visible pixels) instead of O(samples).
 *
 * Layout: level k stores min/max per bucket of (256 << k) samples.
 * Level 0 buckets = 256 samples. A 10-minute stereo clip at 44.1k needs
 * only ~103K floats total for all levels — a few hundred KB.
 *
 * C11, opaque, self-contained.
 */
#ifndef WUBUS_WBUS_WAVCACHE_H
#define WUBUS_WBUS_WAVCACHE_H

#include <stdint.h>
#include <stddef.h>
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_wavcache wb_wavcache;

/* Build a full pyramid from mono PCM. Returns NULL on alloc failure. */
wb_wavcache *wb_wavcache_build(const wb_sample *pcm, uint32_t n);

void wb_wavcache_free(wb_wavcache *c);

/* Draw range [t0,t1) (in SAMPLES) into `out_pixels` vertical min/max pairs.
 * Picks the best pyramid level automatically for pixel_count resolution.
 * Each output pair is {min,max} in -1..1; out_pixels pairs are written
 * contiguously. Returns number of pixel columns written. */
int wb_wavcache_range(const wb_wavcache *c,
                      double t0, double t1,
                      float *out_min, float *out_max, int pixel_count);

/* Total sample count backing this cache. */
uint32_t wb_wavcache_length(const wb_wavcache *c);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_WAVCACHE_H */
