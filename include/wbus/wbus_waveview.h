/* wbus_waveview.h — R074 hop 169 (G-SF077): waveform view data.
 * Min/max peak pairs per bucket for drawing waveforms headless. */
#ifndef WUBUS_WAVEVIEW_H
#define WUBUS_WAVEVIEW_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    float min, max;
} wb_wave_bucket;

/* Compute buckets from interleaved mono f32 samples.
 * n = sample count; nb = bucket count (buckets cover [0,n)).
 * Returns 0 on success. */
int wb_wave_peaks(const float *samples, size_t n, int nb,
                  wb_wave_bucket *out);

#endif /* WUBUS_WAVEVIEW_H */
