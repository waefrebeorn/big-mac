/* wb_waveview.c — R074 hop 169 (G-SF077): waveform peak buckets for
 * the arrangement view / stem inspection. Pure C11. */
#include "wbus/wbus_waveview.h"

int wb_wave_peaks(const float *samples, size_t n, int nb,
                  wb_wave_bucket *out) {
    if (!samples || !out || nb <= 0) return -1;
    size_t per = n / (size_t)nb;
    if (per == 0) return -1;
    for (int b = 0; b < nb; b++) {
        float mn = 1e30f, mx = -1e30f;
        const float *s = samples + (size_t)b * per;
        for (size_t i = 0; i < per; i++) {
            if (s[i] < mn) mn = s[i];
            if (s[i] > mx) mx = s[i];
        }
        out[b].min = mn;
        out[b].max = mx;
    }
    return 0;
}
