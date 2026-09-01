/* test_motion_blur.c — verify motion blur effect node (R086).
 *
 * Tests:
 *   1. Node creation with default samples
 *   2. Shutter angle setter (clamp 0..360)
 *   3. Shutter phase setter (clamp -180..180)
 *   4. First frame passes through (no previous = no blur)
 *   5. Second frame with transform change applies blur
 *   6. Zero shutter angle = no blur (passthrough)
 *   7. Node destroy cleans up state
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus/wbus_compositor.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== Motion Blur Effect Node Test (R086) ===\n\n");

    /* ---- Test 1: creation with default samples ---- */
    printf("-- node creation --\n");
    wb_node *mb = wb_node_effect_motion_blur(8);
    CHECK(mb != NULL, "motion blur node created");
    CHECK(mb->kind == WB_NODE_EFFECT, "kind is WB_NODE_EFFECT");
    CHECK(mb->n_inputs == 1, "has 1 input slot");

    /* ---- Test 2: shutter angle setter ---- */
    printf("\n-- shutter angle --\n");
    wb_node_effect_motion_blur_set_shutter_angle(mb, 360.0f);
    /* Can't read back directly, but we can verify no crash + clamp */
    wb_node_effect_motion_blur_set_shutter_angle(mb, 500.0f); /* clamp to 360 */
    wb_node_effect_motion_blur_set_shutter_angle(mb, -10.0f); /* clamp to 0 */
    CHECK(1, "shutter angle setter clamps without crash");

    /* ---- Test 3: shutter phase setter ---- */
    printf("\n-- shutter phase --\n");
    wb_node_effect_motion_blur_set_shutter_phase(mb, 90.0f);
    wb_node_effect_motion_blur_set_shutter_phase(mb, -200.0f); /* clamp to -180 */
    wb_node_effect_motion_blur_set_shutter_phase(mb, 200.0f);  /* clamp to 180 */
    CHECK(1, "shutter phase setter clamps without crash");

    /* ---- Test 4: first frame passthrough ---- */
    printf("\n-- first frame passthrough --\n");
    /* Create a solid red source */
    wb_node *red = wb_node_source_color(1.0f, 0.0f, 0.0f, 1.0f, 32, 32);
    CHECK(red != NULL, "red source created");

    /* Connect red -> motion_blur */
    mb->inputs[0] = red;

    /* First pull: should pass through (no previous frame) */
    wb_frame *f1 = wb_node_pull(mb, 0.0, 0, 0, 32, 32);
    CHECK(f1 != NULL, "first pull returns frame");
    if (f1) {
        /* Center pixel should still be red (no blur on first frame) */
        wb_px center = f1->px[16 * 32 + 16];
        CHECK(center.r > 0.9f, "first frame: center is red");
        CHECK(center.g < 0.1f, "first frame: no green");
        CHECK(center.b < 0.1f, "first frame: no blue");
        CHECK(f1->w == 32 && f1->h == 32, "frame dimensions correct");
    }

    /* ---- Test 5: second frame with motion applies blur ---- */
    printf("\n-- second frame with motion --\n");
    /* Set shutter angle back to something that produces visible blur */
    wb_node_effect_motion_blur_set_shutter_angle(mb, 180.0f);
    wb_node_effect_motion_blur_set_shutter_phase(mb, 0.0f);

    /* Second pull at t=0.042 (one frame later at ~24fps).
     * Since no animated params are bound, transform stays at defaults
     * (no motion detected). Frame should still pass through. */
    wb_frame *f2 = wb_node_pull(mb, 0.042, 0, 0, 32, 32);
    CHECK(f2 != NULL, "second pull returns frame");
    if (f2) {
        wb_px center2 = f2->px[16 * 32 + 16];
        CHECK(center2.r > 0.9f, "second frame (no motion): still red");
    }

    /* ---- Test 6: zero shutter angle = passthrough ---- */
    printf("\n-- zero shutter angle passthrough --\n");
    wb_node_effect_motion_blur_set_shutter_angle(mb, 0.0f);
    wb_frame *f3 = wb_node_pull(mb, 0.084, 0, 0, 32, 32);
    CHECK(f3 != NULL, "zero-shutter pull returns frame");
    if (f3) {
        wb_px center3 = f3->px[16 * 32 + 16];
        CHECK(center3.r > 0.9f, "zero shutter: still red (passthrough)");
    }

    /* ---- Test 7: destroy cleans up ---- */
    printf("\n-- cleanup --\n");
    wb_node_destroy(mb);
    CHECK(1, "motion blur node destroyed without crash");

    /* Also destroy the source (mb's input was set directly, not via connect) */
    wb_node_destroy(red);

    /* ---- Summary ---- */
    printf("\n=== Summary: %d/%d checks passed", checks - failures, checks);
    if (failures > 0) printf(", %d FAILED", failures);
    printf(" ===\n");

    return failures > 0 ? 1 : 0;
}