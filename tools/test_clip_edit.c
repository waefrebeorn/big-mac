/* test_clip_edit.c — R043 (G1/G2) clip-edit side-table gate.
 * Verifies the opaque table: create/destroy, neutral default, get/set,
 * and the fade envelope math (matches the render path). C11, links only
 * wb_clip_edit.o (self-contained module). */

#include <stdio.h>
#include <math.h>
#include "wbus/wbus_clip_edit.h"

static int checks = 0, failures = 0;
#define CHECK(c, m) do { checks++; if (!(c)) { failures++; fprintf(stderr, "  [FAIL] %s\n", m); } } while(0)

int main(void) {
    wb_clip_edit_table *t = wb_clip_edit_create();
    CHECK(t != NULL, "clip-edit table created");

    /* neutral default: no fade, zero offset, env = 1.0 */
    wb_clip_edit *e = wb_clip_edit_get(t, 0, 0);
    CHECK(e != NULL, "get(0,0) returns entry");
    CHECK(e->fade_in == 0.0f && e->fade_out == 0.0f, "default fades are 0");
    CHECK(e->start_in_source == 0.0, "default offset 0");
    CHECK(fabsf(wb_clip_edit_env(e, 100.0, 44100.0, 44100.0) - 1.0f) < 1e-4f,
          "neutral env = 1.0");

    /* set a 1s fade-in over a 4s clip */
    e->fade_in = 1.0f;
    /* at t=0 -> env 0 (silent edge); at t=0.5s -> 0.5; at t>=1s -> 1.0 */
    double sr = 44100.0;
    CHECK(fabsf(wb_clip_edit_env(e, 0.0*sr, 4.0*sr, sr) - 0.0f) < 1e-4f, "fade-in @0 = 0");
    CHECK(fabsf(wb_clip_edit_env(e, 0.5*sr, 4.0*sr, sr) - 0.5f) < 1e-3f, "fade-in @0.5s = 0.5");
    CHECK(fabsf(wb_clip_edit_env(e, 2.0*sr, 4.0*sr, sr) - 1.0f) < 1e-4f, "fade-in @2s = 1.0");

    /* fade-out: 1s at tail of 4s clip */
    e->fade_out = 1.0f;
    CHECK(fabsf(wb_clip_edit_env(e, 3.5*sr, 4.0*sr, sr) - 0.5f) < 1e-3f, "fade-out @3.5s = 0.5");
    CHECK(fabsf(wb_clip_edit_env(e, 3.9*sr, 4.0*sr, sr) - 0.1f) < 1e-3f, "fade-out @3.9s = 0.1");

    /* independent tracks/clips don't collide */
    wb_clip_edit *e2 = wb_clip_edit_get(t, 1, 0);
    CHECK(e2 != e, "different track -> different entry");
    CHECK(e2->fade_in == 0.0f, "track 1 neutral despite track 0 having fades");
    wb_clip_edit *e3 = wb_clip_edit_get(t, 0, 1);
    CHECK(e3 != e && e3->fade_in == 0.0f, "different clip -> neutral");

    /* clear resets */
    wb_clip_edit_clear(t, 0, 0);
    e = wb_clip_edit_get(t, 0, 0);
    CHECK(e->fade_in == 0.0f && e->fade_out == 0.0f, "clear restores neutral");

    /* NULL-safe env */
    CHECK(fabsf(wb_clip_edit_env(NULL, 100.0, 44100.0, sr) - 1.0f) < 1e-4f,
          "NULL edit -> env 1.0");

    wb_clip_edit_destroy(t);
    t = NULL;

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
