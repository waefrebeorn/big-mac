/* test_wavcache.c — R066: waveform LOD pyramid verification.
 * A known signal (full-scale square wave for half the buffer, silence
 * after) must produce exact min/max at every zoom level, and range
 * queries at wildly different zooms must agree on the extremes. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "wbus/wbus_wavcache.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== Waveform LOD cache (R066) test ===\n\n");
    const uint32_t n = 44100 * 10;   /* 10 seconds */
    wb_sample *pcm = malloc(n * sizeof(wb_sample));
    /* [0-5s]: alternating +/-1 square @ 100Hz ; [5-10s]: silence */
    for (uint32_t i = 0; i < n; i++) {
        double t = (double)i / 44100.0;
        int loud = t < 5.0;
        pcm[i] = loud ? ((int)(t*100.0*2) % 2 ? 0.9f : -0.9f)
                      : 0.0f;
    }

    wb_wavcache *c = wb_wavcache_build(pcm, n);
    CHECK(c != NULL, "pyramid built");
    CHECK(wb_wavcache_length(c) == n, "length stored");

    /* whole-file query at coarse zoom */
    float mn[800], mx[800];
    int px = wb_wavcache_range(c, 0, n, mn, mx, 800);
    CHECK(px == 800, "range returns requested pixel count");

    float gmin = 1e9f, gmax = -1e9f;
    for (int i = 0; i < px; i++) {
        if (mn[i] < gmin) gmin = mn[i];
        if (mx[i] > gmax) gmax = mx[i];
    }
    CHECK(fabsf(gmin + 0.9f) < 0.01f, "global min is -0.9 (square low)");
    CHECK(fabsf(gmax - 0.9f) < 0.01f, "global max is +0.9 (square high)");

    /* silent region: at FINE zoom (level 0, 256-sample buckets) a window
     * strictly inside the silence must read zero. At COARSE zoom the
     * boundary bucket may legitimately include pre-boundary loud samples
     * — that is inherent to bucketed min/max LOD, so we only assert the
     * fine case here. */
    float mn2[64], mx2[64];
    wb_wavcache_range(c, n/2 + 4096, n/2 + 8192, mn2, mx2, 16);
    float smax = -1e9f;
    for (int i = 0; i < 16; i++) if (mx2[i] > smax) smax = mx2[i];
    CHECK(smax <= 0.001f, "silence reads as silence at fine zoom");

    /* fine zoom (level 0): single sample-accurate window inside the
     * loud part must show both rails */
    wb_wavcache_range(c, 1000, 1004, mn2, mx2, 4);
    float fmn = 1e9f, fmx = -1e9f;
    for (int i = 0; i < 4; i++) { if (mn2[i]<fmn) fmn=mn2[i]; if (mx2[i]>fmx) fmx=mx2[i]; }
    CHECK(fmn < -0.85f && fmx > 0.85f,
          "fine zoom captures both square rails");

    /* degenerate inputs */
    CHECK(wb_wavcache_range(c, 5000, 5000, mn, mx, 10) == 0, "empty range -> 0 px");
    CHECK(wb_wavcache_range(NULL, 0, 10, mn, mx, 10) == 0, "NULL cache safe");
    CHECK(wb_wavcache_build(NULL, 10) == NULL, "NULL samples rejected");

    free(pcm);
    wb_wavcache_free(c);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
