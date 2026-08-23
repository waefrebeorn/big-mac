/* wb_wavcache.c — waveform LOD pyramid (R066). */

#include "wbus/wbus_wavcache.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WB_WC_BASE 256          /* level-0 bucket size */
#define WB_WC_LEVELS 8          /* 256 .. 32768 samples per bucket */

struct wb_wavcache {
    uint32_t n;                              /* total samples */
    uint32_t buckets[WB_WC_LEVELS];          /* bucket count per level */
    float *mins[WB_WC_LEVELS];
    float *maxs[WB_WC_LEVELS];
};

wb_wavcache *wb_wavcache_build(const wb_sample *pcm, uint32_t n) {
    if (!pcm || n == 0) return NULL;
    wb_wavcache *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->n = n;

    uint32_t bs = WB_WC_BASE;
    for (int L = 0; L < WB_WC_LEVELS; L++) {
        uint32_t nb = (n + bs - 1) / bs;
        c->buckets[L] = nb;
        c->mins[L] = malloc(nb * sizeof(float));
        c->maxs[L] = malloc(nb * sizeof(float));
        if (!c->mins[L] || !c->maxs[L]) { wb_wavcache_free(c); return NULL; }

        if (L == 0) {
            /* scan raw samples */
            for (uint32_t b = 0; b < nb; b++) {
                uint32_t i0 = b*bs, i1 = i0 + bs; if (i1 > n) i1 = n;
                float mn = 1e9f, mx = -1e9f;
                for (uint32_t i = i0; i < i1; i++) {
                    float v = pcm[i];
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
                c->mins[0][b] = mn; c->maxs[0][b] = mx;
            }
        } else {
            /* derive from previous level: each parent bucket covers
             * 2 children */
            uint32_t pb = c->buckets[L-1];
            for (uint32_t b = 0; b < nb; b++) {
                uint32_t c0 = b*2, c1 = c0+2; if (c1 > pb) c1 = pb;
                if (c0 >= pb) { c->mins[L][b]=c->mins[L-1][pb-1];
                                c->maxs[L][b]=c->maxs[L-1][pb-1]; continue; }
                float mn = 1e9f, mx = -1e9f;
                for (uint32_t k = c0; k < c1; k++) {
                    if (c->mins[L-1][k] < mn) mn = c->mins[L-1][k];
                    if (c->maxs[L-1][k] > mx) mx = c->maxs[L-1][k];
                }
                c->mins[L][b] = mn; c->maxs[L][b] = mx;
            }
        }
        bs <<= 1;
    }
    return c;
}

void wb_wavcache_free(wb_wavcache *c) {
    if (!c) return;
    for (int L = 0; L < WB_WC_LEVELS; L++) {
        free(c->mins[L]); free(c->maxs[L]);
    }
    free(c);
}

uint32_t wb_wavcache_length(const wb_wavcache *c) { return c ? c->n : 0; }

int wb_wavcache_range(const wb_wavcache *c, double t0, double t1,
                      float *out_min, float *out_max, int pixel_count) {
    if (!c || !out_min || !out_max || pixel_count <= 0) return 0;
    if (t0 < 0) t0 = 0;
    if (t1 > (double)c->n) t1 = c->n;
    if (t1 <= t0) return 0;

    double span = t1 - t0;
    double samples_per_pixel = span / pixel_count;

    /* pick level: smallest bucket size >= samples_per_pixel/2-ish */
    int L = 0;
    uint32_t bs = WB_WC_BASE;
    while (L + 1 < WB_WC_LEVELS && bs < samples_per_pixel) { bs <<= 1; L++; }

    uint32_t nb = c->buckets[L];
    for (int px = 0; px < pixel_count; px++) {
        double s0 = t0 + (double)px * span / pixel_count;
        double s1 = s0 + span / pixel_count;
        uint32_t b0 = (uint32_t)(s0 / bs);
        uint32_t b1 = (uint32_t)(s1 / bs);
        if (b1 >= nb) b1 = nb ? nb-1 : 0;
        if (b0 > b1) b0 = b1;
        float mn = c->mins[L][b0], mx = c->maxs[L][b0];
        for (uint32_t b = b0+1; b <= b1 && b < nb; b++) {
            if (c->mins[L][b] < mn) mn = c->mins[L][b];
            if (c->maxs[L][b] > mx) mx = c->maxs[L][b];
        }
        out_min[px] = mn; out_max[px] = mx;
    }
    return pixel_count;
}
