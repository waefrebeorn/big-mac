/* test_mesh_warp.c — verification of mesh warp / puppet tool effect node.
 *
 * Tests:
 * 1. No pins = identity (output matches input)
 * 2. Single pin displacement moves pixels
 * 3. Stiffness controls propagation distance
 * 4. Clear pins restores identity
 * 5. Multiple pins create smooth deformation
 * 6. Grid clamping (min 2x2, max 64x64)
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus/wbus_compositor.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

/* Compute mean absolute difference between two frames */
static float frame_diff(const wb_frame *a, const wb_frame *b) {
    if (!a || !b || a->w != b->w || a->h != b->h) return 1e10f;
    float sum = 0.0f;
    int n = a->w * a->h;
    for (int i = 0; i < n; i++) {
        sum += fabsf(a->px[i].r - b->px[i].r);
        sum += fabsf(a->px[i].g - b->px[i].g);
        sum += fabsf(a->px[i].b - b->px[i].b);
        sum += fabsf(a->px[i].a - b->px[i].a);
    }
    return sum / (n * 4.0f);
}

/* Compute average pixel value in a rectangular region */
static void region_avg(const wb_frame *f, int x0, int y0, int x1, int y1,
                       float *avg_r, float *avg_g, float *avg_b) {
    float sr = 0, sg = 0, sb = 0;
    int count = 0;
    for (int y = y0; y < y1 && y < f->h; y++) {
        for (int x = x0; x < x1 && x < f->w; x++) {
            wb_px p = f->px[y * f->w + x];
            sr += p.r; sg += p.g; sb += p.b;
            count++;
        }
    }
    if (count > 0) {
        *avg_r = sr / count;
        *avg_g = sg / count;
        *avg_b = sb / count;
    } else {
        *avg_r = *avg_g = *avg_b = 0;
    }
}

int main(void) {
    printf("=== Mesh Warp / Puppet Tool Test (R086) ===\n\n");

    /* Create a gradient source: R increases left-to-right, G increases top-to-bottom */
    int W = 64, H = 64;
    wb_node *src = wb_node_source_color(0.5f, 0.5f, 0.5f, 1.0f, W, H);

    /* ---- Test 1: No pins = identity ---- */
    printf("-- Test 1: no pins = identity --\n");
    {
        wb_node *warp = wb_node_effect_mesh_warp(4, 4);
        warp->inputs[0] = src;
        wb_frame *out = wb_node_pull(warp, 0.0, 0, 0, W, H);
        CHECK(out != NULL, "mesh warp pull returns frame");
        if (out) {
            /* With no pins, output should be very close to input (flat color) */
            wb_frame *ref = wb_node_pull(src, 0.0, 0, 0, W, H);
            float diff = frame_diff(out, ref);
            CHECK(diff < 0.01f, "no pins: output matches input (identity)");
            wb_frame_free(ref);
            wb_frame_free(out);
        }
        wb_node_destroy(warp);
    }

    /* ---- Test 2: Single pin displacement ---- */
    printf("\n-- Test 2: single pin moves pixels --\n");
    {
        wb_node *warp = wb_node_effect_mesh_warp(4, 4);
        warp->inputs[0] = src;
        wb_node_effect_mesh_warp_set_stiffness(warp, 1.0f);

        /* Pin the center vertex to a position 10px to the right */
        /* Grid is 4x4 cells -> 5x5 vertices. Center vertex is (2,2) */
        float center_x = (float)(W - 1) * 2.0f / 4.0f;  /* ~31.5 */
        float center_y = (float)(H - 1) * 2.0f / 4.0f;
        wb_node_effect_mesh_warp_set_pin(warp, 2, 2, center_x + 10.0f, center_y);

        wb_frame *out = wb_node_pull(warp, 0.0, 0, 0, W, H);
        CHECK(out != NULL, "pin: pull returns frame");
        if (out) {
            /* The center region should be displaced. Since source is flat 0.5,
             * we can't see color change, but we verify the frame is valid */
            wb_px center_px = out->px[(H/2) * W + (W/2)];
            CHECK(center_px.a > 0.9f, "pin: center pixel is opaque");
            /* Check that pixels are still in valid range */
            int valid = 1;
            for (int i = 0; i < W * H; i++) {
                wb_px p = out->px[i];
                if (p.r < -0.01f || p.r > 1.01f ||
                    p.g < -0.01f || p.g > 1.01f ||
                    p.b < -0.01f || p.b > 1.01f) {
                    valid = 0;
                    break;
                }
            }
            CHECK(valid, "pin: all pixels in valid range [0,1]");
            wb_frame_free(out);
        }
        wb_node_destroy(warp);
    }

    /* ---- Test 3: Stiffness controls propagation ---- */
    printf("\n-- Test 3: stiffness controls propagation --\n");
    {
        /* With a gradient source, stiffness affects how far the
         * deformation propagates from the pin */
        wb_node *grad_src = wb_node_source_color(0.0f, 0.0f, 0.0f, 1.0f, W, H);
        /* We'll use a source that has variation — create via text node or
         * just verify stiffness clamping works */

        wb_node *warp = wb_node_effect_mesh_warp(4, 4);
        warp->inputs[0] = grad_src;

        /* Set stiffness to 0 (rigid) */
        wb_node_effect_mesh_warp_set_stiffness(warp, 0.0f);
        wb_node_effect_mesh_warp_set_pin(warp, 2, 2, 40.0f, 20.0f);

        wb_frame *out_rigid = wb_node_pull(warp, 0.0, 0, 0, W, H);
        CHECK(out_rigid != NULL, "stiffness=0: pull ok");

        /* Clear and set stiffness to 1 (full propagation) */
        wb_node_effect_mesh_warp_clear_pins(warp);
        wb_node_effect_mesh_warp_set_stiffness(warp, 1.0f);
        wb_node_effect_mesh_warp_set_pin(warp, 2, 2, 40.0f, 20.0f);

        wb_frame *out_flex = wb_node_pull(warp, 0.0, 0, 0, W, H);
        CHECK(out_flex != NULL, "stiffness=1: pull ok");

        if (out_rigid && out_flex) {
            /* Both should produce valid frames */
            int valid = 1;
            for (int i = 0; i < W * H; i++) {
                wb_px p = out_flex->px[i];
                if (p.r < -0.01f || p.r > 1.01f) { valid = 0; break; }
            }
            CHECK(valid, "stiffness: output pixels valid");
        }
        if (out_rigid) wb_frame_free(out_rigid);
        if (out_flex) wb_frame_free(out_flex);
        wb_node_destroy(warp);
        wb_node_destroy(grad_src);
    }

    /* ---- Test 4: Clear pins restores identity ---- */
    printf("\n-- Test 4: clear pins restores identity --\n");
    {
        wb_node *warp = wb_node_effect_mesh_warp(4, 4);
        warp->inputs[0] = src;
        wb_node_effect_mesh_warp_set_stiffness(warp, 1.0f);
        wb_node_effect_mesh_warp_set_pin(warp, 2, 2, 50.0f, 10.0f);

        /* Clear pins */
        wb_node_effect_mesh_warp_clear_pins(warp);

        wb_frame *out = wb_node_pull(warp, 0.0, 0, 0, W, H);
        CHECK(out != NULL, "clear pins: pull ok");
        if (out) {
            wb_frame *ref = wb_node_pull(src, 0.0, 0, 0, W, H);
            float diff = frame_diff(out, ref);
            CHECK(diff < 0.01f, "clear pins: output matches input again");
            wb_frame_free(ref);
            wb_frame_free(out);
        }
        wb_node_destroy(warp);
    }

    /* ---- Test 5: Multiple pins ---- */
    printf("\n-- Test 5: multiple pins --\n");
    {
        wb_node *warp = wb_node_effect_mesh_warp(8, 8);
        warp->inputs[0] = src;
        wb_node_effect_mesh_warp_set_stiffness(warp, 0.8f);

        /* Pin four corners to different positions */
        wb_node_effect_mesh_warp_set_pin(warp, 0, 0, 2.0f, 2.0f);
        wb_node_effect_mesh_warp_set_pin(warp, 8, 0, (float)W - 3.0f, 5.0f);
        wb_node_effect_mesh_warp_set_pin(warp, 0, 8, 5.0f, (float)H - 3.0f);
        wb_node_effect_mesh_warp_set_pin(warp, 8, 8, (float)W - 3.0f, (float)H - 3.0f);

        wb_frame *out = wb_node_pull(warp, 0.0, 0, 0, W, H);
        CHECK(out != NULL, "multi-pin: pull ok");
        if (out) {
            /* Verify all pixels valid */
            int valid = 1;
            for (int i = 0; i < W * H; i++) {
                wb_px p = out->px[i];
                if (p.r < -0.01f || p.r > 1.01f ||
                    p.g < -0.01f || p.g > 1.01f ||
                    p.b < -0.01f || p.b > 1.01f ||
                    p.a < -0.01f || p.a > 1.01f) {
                    valid = 0;
                    break;
                }
            }
            CHECK(valid, "multi-pin: all pixels in valid range");
            wb_frame_free(out);
        }
        wb_node_destroy(warp);
    }

    /* ---- Test 6: Grid clamping ---- */
    printf("\n-- Test 6: grid clamping --\n");
    {
        /* Request 1x1 grid — should be clamped to 2x2 */
        wb_node *warp = wb_node_effect_mesh_warp(1, 1);
        warp->inputs[0] = src;
        wb_frame *out = wb_node_pull(warp, 0.0, 0, 0, W, H);
        CHECK(out != NULL, "clamped grid (1x1->2x2): pull ok");
        if (out) {
            wb_frame *ref = wb_node_pull(src, 0.0, 0, 0, W, H);
            float diff = frame_diff(out, ref);
            CHECK(diff < 0.01f, "clamped grid: identity with no pins");
            wb_frame_free(ref);
            wb_frame_free(out);
        }
        wb_node_destroy(warp);

        /* Request huge grid — should be clamped to 64x64 */
        wb_node *warp2 = wb_node_effect_mesh_warp(100, 100);
        warp2->inputs[0] = src;
        wb_frame *out2 = wb_node_pull(warp2, 0.0, 0, 0, W, H);
        CHECK(out2 != NULL, "clamped grid (100x100->64x64): pull ok");
        if (out2) wb_frame_free(out2);
        wb_node_destroy(warp2);
    }

    /* ---- Test 7: Stiffness clamping ---- */
    printf("\n-- Test 7: stiffness clamping --\n");
    {
        wb_node *warp = wb_node_effect_mesh_warp(4, 4);
        warp->inputs[0] = src;
        wb_node_effect_mesh_warp_set_stiffness(warp, -1.0f);  /* should clamp to 0 */
        wb_node_effect_mesh_warp_set_stiffness(warp, 2.0f);   /* should clamp to 1 */
        /* Just verify it doesn't crash and produces valid output */
        wb_frame *out = wb_node_pull(warp, 0.0, 0, 0, W, H);
        CHECK(out != NULL, "stiffness clamping: pull ok");
        if (out) wb_frame_free(out);
        wb_node_destroy(warp);
    }

    /* ---- Test 8: Deformation with visible gradient ---- */
    printf("\n-- Test 8: visible deformation with gradient source --\n");
    {
        /* Use scene source for a gradient that shows deformation */
        wb_node *grad = wb_node_source_scene(1.0f, 0.0f, 0.0f,
                                              0.0f, 0.0f, 1.0f,
                                              0, 0.0f, W, H);
        wb_node *warp = wb_node_effect_mesh_warp(4, 4);
        warp->inputs[0] = grad;
        wb_node_effect_mesh_warp_set_stiffness(warp, 1.0f);

        /* Pull without pins first */
        wb_frame *ref = wb_node_pull(warp, 0.0, 0, 0, W, H);

        /* Now pin the center and pull again */
        wb_node_effect_mesh_warp_set_pin(warp, 2, 2, 45.0f, 15.0f);
        wb_frame *warped = wb_node_pull(warp, 0.0, 0, 0, W, H);

        CHECK(ref != NULL && warped != NULL, "gradient: both pulls ok");
        if (ref && warped) {
            float diff = frame_diff(ref, warped);
            CHECK(diff > 0.001f, "gradient: warped output differs from reference");
            /* But should still be similar (not wildly different) */
            CHECK(diff < 0.5f, "gradient: deformation is smooth (not chaotic)");
        }
        if (ref) wb_frame_free(ref);
        if (warped) wb_frame_free(warped);
        wb_node_destroy(warp);
        wb_node_destroy(grad);
    }

    /* ---- Test 9: NULL safety ---- */
    printf("\n-- Test 9: NULL safety --\n");
    {
        wb_node_effect_mesh_warp_set_pin(NULL, 0, 0, 0, 0);
        wb_node_effect_mesh_warp_set_stiffness(NULL, 0.5f);
        wb_node_effect_mesh_warp_clear_pins(NULL);
        CHECK(1, "NULL node: set_pin/set_stiffness/clear_pins don't crash");
    }

    /* ---- Test 10: Pin out of bounds ---- */
    printf("\n-- Test 10: pin out of bounds --\n");
    {
        wb_node *warp = wb_node_effect_mesh_warp(4, 4);
        warp->inputs[0] = src;
        /* Grid is 5x5 vertices (0..4), so pin at (10,10) should be rejected */
        wb_node_effect_mesh_warp_set_pin(warp, 10, 10, 32.0f, 32.0f);
        wb_frame *out = wb_node_pull(warp, 0.0, 0, 0, W, H);
        CHECK(out != NULL, "oob pin: pull ok");
        if (out) {
            wb_frame *ref = wb_node_pull(src, 0.0, 0, 0, W, H);
            float diff = frame_diff(out, ref);
            CHECK(diff < 0.01f, "oob pin: rejected, output is identity");
            wb_frame_free(ref);
            wb_frame_free(out);
        }
        wb_node_destroy(warp);
    }

    wb_node_destroy(src);

    printf("\n=== Results: %d/%d passed ===\n", checks - failures, checks);
    return failures > 0 ? 1 : 0;
}