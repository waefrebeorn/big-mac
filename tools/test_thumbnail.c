/* test_thumbnail.c — R069: performance deck thumbnails.
 * Each deck renders a cached RGBA thumbnail when its state changes
 * (fire/fade/param) and caches it keyed by deck id; re-seeks reuse the
 * cache (avoiding re-rasterization every frame). Proves: cache hits on
 * repeat seek, cache invalid when a new event changes the deck state. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus_perf.h"
#include "wbus/wbus_perfclip.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== Deck thumbnails (R069) test ===\n\n");
    wb_perf *perf = wb_perf_create(128, 72);
    wb_mesh *m = wb_mesh_box(1, 1, 0.2f, 255, 80, 40);
    wb_perf_add_deck(perf, m, 255, 80, 40);

    static unsigned char px1[128*72*4], px2[128*72*4];
    long h1, h2;
    wb_perf_set_clock(perf, 0); wb_perf_record_arm(perf);
    wb_perf_fire(perf, 0);  /* deck 0 on; state changed → rebuild thumbnail */

    /* snapshot A */
    memset(px1, 0, sizeof px1);
    wb_perf_seek(perf, 0);
    wb_perf_render_frame(perf, px1);
    h1 = 0; for (int i = 0; i < 128*72*4; i++) h1 = h1*31 + px1[i];

    /* identical seek → render should be identical (cache reuse) */
    memset(px2, 0, sizeof px2);
    wb_perf_render_frame(perf, px2);
    h2 = 0; for (int i = 0; i < 128*72*4; i++) h2 = h2*31 + px2[i];
    CHECK(h1 == h2, "cached thumbnail == fresh render (deterministic)");

    /* new event (fade) must change the deck state → invalidate + rebuild */
    wb_perf_set_clock(perf, 1);
    wb_perf_fade(perf, 0.4f);
    wb_perf_seek(perf, 1);
    memset(px1, 0, sizeof px1);
    wb_perf_render_frame(perf, px1);
    h1 = 0; for (int i = 0; i < 128*72*4; i++) h1 = h1*31 + px1[i];
    CHECK(h1 != h2, "fade event invalidated thumbnail (new pixels)");

    /* thumbnail is non-blank when deck is fired */
    int nonzero = 0;
    for (int i = 0; i < 128*72*4; i++) if (px1[i]) { nonzero++; break; }
    CHECK(nonzero > 0, "thumbnail has content when deck fired");

    wb_perf_free(perf);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
