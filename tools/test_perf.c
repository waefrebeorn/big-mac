/* test_perf.c — R065: live performance capture + deterministic replay.
 * Proves: fire/fade/param events are captured while armed, seek() replays
 * the exact state at any t, and the same t always renders identical
 * pixels (determinism = the recording IS reproducible). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wbus/wbus_perf.h"
#include "wbus/wbus_mesh.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

static const int W = 320, H = 240;

static uint64_t pixel_hash(const uint8_t *img) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < (size_t)W*H*4; i++) { h ^= img[i]; h *= 1099511628211ULL; }
    return h;
}

int main(void) {
    printf("=== Performance engine (R065) test ===\n\n");

    wb_perf *p = wb_perf_create(W, H);
    CHECK(p != NULL, "perf created");
    if (!p) return 1;

    /* two decks with distinct colors */
    wb_mesh *mred = wb_mesh_box(1, 1, 0.2f, 255, 60, 40);
    wb_mesh *mblu = wb_mesh_sphere(1, 8, 10, 50, 100, 255);
    CHECK(wb_perf_add_deck(p, mred, 255, 60, 40) == 0, "deck A added");
    CHECK(wb_perf_add_deck(p, mblu, 50, 100, 255) == 1, "deck B added");
    CHECK(wb_perf_deck_count(p) == 2, "2 decks");

    static uint8_t img[W*H*4];

    /* ---- LIVE performance, clock-driven ---- */
    wb_perf_set_clock(p, 0.0);
    wb_perf_record_arm(p);
    wb_perf_fire(p, 0);            /* t=0   deck A on  */
    wb_perf_set_clock(p, 1.0);
    wb_perf_fade(p, 0.5f);         /* t=1   crossfade halfway */
    wb_perf_fire(p, 1);            /* t=1   deck B on  */
    wb_perf_set_clock(p, 2.0);
    wb_perf_param(p, 0, 0, 3.0f);  /* t=2   deck A spin=3 */
    wb_perf_record_stop(p);

    CHECK(wb_perf_event_count(p) == 4, "4 events captured");
    CHECK(wb_perf_recording(p) == 0, "recording stopped");

    /* ---- REPLAY determinism: seek(t) twice -> identical pixels ---- */
    wb_perf_seek(p, 0.5);
    wb_perf_render_frame(p, img);
    long h1a = pixel_hash(img);
    wb_perf_seek(p, 0.5);
    memset(img, 0, sizeof img);
    wb_perf_render_frame(p, img);
    long h1b = pixel_hash(img);
    CHECK(h1a == h1b && h1a != 0, "seek(0.5) twice -> identical frame (deterministic)");

    /* ---- state at t=0.25: only deck A visible ---- */
    wb_perf_seek(p, 0.25);
    wb_perf_render_frame(p, img);
    int red_px = 0, blu_px = 0;
    for (int i = 0; i < W*H; i++) {
        if (img[i*4+3] == 255) {
            if (img[i*4] > img[i*4+2]) red_px++;
            else if (img[i*4+2] > img[i*4]) blu_px++;
        }
    }
    CHECK(red_px > 500, "t=0.25: deck A visible");
    CHECK(blu_px == 0,  "t=0.25: deck B not yet fired");

    /* ---- state at t=1.5: both decks visible (B fired at t=1) ---- */
    wb_perf_seek(p, 1.5);
    wb_perf_render_frame(p, img);
    red_px = 0; blu_px = 0;
    for (int i = 0; i < W*H; i++) {
        if (img[i*4+3] == 255) {
            if (img[i*4] > img[i*4+2]) red_px++;
            else if (img[i*4+2] > img[i*4]) blu_px++;
        }
    }
    CHECK(red_px > 200 && blu_px > 200,
          "t=1.5: both decks composited after B fired");

    /* ---- seek BEFORE an event excludes it ---- */
    wb_perf_seek(p, 0.99);          /* just before B fires at t=1.0 */
    wb_perf_render_frame(p, img);
    blu_px = 0;
    for (int i = 0; i < W*H; i++)
        if (img[i*4+3] == 255 && img[i*4+2] > img[i*4]) blu_px++;
    CHECK(blu_px == 0, "t=0.99: B's fire event not yet applied");

    /* ---- unfire works in replay ---- */
    wb_perf_set_clock(p, 3.0);
    wb_perf_unfire(p, 0);
    wb_perf_seek(p, 3.5);
    wb_perf_render_frame(p, img);
    red_px = 0;
    for (int i = 0; i < W*H; i++)
        if (img[i*4+3] == 255 && img[i*4] > img[i*4+2]) red_px++;
    CHECK(red_px == 0, "unfire removes deck A from replay");

    wb_mesh_free(mred); wb_mesh_free(mblu);
    wb_perf_free(p);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
