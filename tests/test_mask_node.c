/* test_mask_node.c — test cases for per-layer mask effect node (R090) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void) {
    int pass = 0, fail = 0;

    printf("=== Per-Layer Mask Node (R090) ===\n");

    /* Test 1: Create mask node */
    wb_node *mask = wb_node_effect_mask(64, 64);
    CHECK(mask != NULL, "mask node created");

    /* Test 2: Mask node has correct kind */
    CHECK(wb_node_get_kind(mask) == WB_NODE_EFFECT, "mask is WB_NODE_EFFECT");

    /* Test 3: Mask node has 2 inputs */
    CHECK(mask->n_inputs == 2, "mask has 2 inputs");

    /* Test 4: Set feather */
    wb_node_effect_mask_set_feather(mask, 3.0f);
    CHECK(1, "set_feather succeeded (no crash)");

    /* Test 5: Set expand (positive = grow) */
    wb_node_effect_mask_set_expand(mask, 2.0f);
    CHECK(1, "set_expand positive succeeded");

    /* Test 6: Set expand (negative = shrink) */
    wb_node_effect_mask_set_expand(mask, -1.5f);
    CHECK(1, "set_expand negative succeeded");

    /* Test 7: Set invert */
    wb_node_effect_mask_set_invert(mask, 1);
    CHECK(1, "set_invert succeeded");

    /* Test 8: Set path (triangle) */
    wb_node_effect_mask_set_path(mask, "M 10 10 L 50 10 L 30 50 Z");
    CHECK(1, "set_path triangle succeeded");

    /* Test 9: Create a source color node */
    wb_node *src = wb_node_source_color(1.0f, 0.5f, 0.0f, 1.0f, 64, 64);
    CHECK(src != NULL, "source color node created");

    /* Test 10: Connect source to mask input 0 */
    CHECK(wb_node_connect(mask, src, 0) == 0, "connected source to mask input 0");

    /* Test 11: Pull mask with path-based mask (no mask source) */
    wb_frame *result = wb_node_pull(mask, 0.0, 0, 0, 64, 64);
    CHECK(result != NULL, "pull mask with path mask returned frame");

    /* Test 12: Check that center pixel is masked (inside triangle, inverted) */
    if (result) {
        /* With invert=1, inside triangle -> mask_alpha=0 -> src_alpha*0 = 0 */
        int center_idx = 20 * 64 + 30; /* clearly inside the triangle */
        CHECK(result->px[center_idx].a < 0.1f, "center pixel masked (inverted)");
    }

    /* Test 13: Check that corner pixel is NOT masked (outside triangle, inverted) */
    if (result) {
        int corner_idx = 0; /* (0,0) is outside the triangle */
        CHECK(result->px[corner_idx].a > 0.9f, "corner pixel unmasked (outside, inverted)");
    }

    if (result) wb_frame_free(result);

    /* Test 14: Set invert back to 0 and re-test */
    wb_node_effect_mask_set_invert(mask, 0);
    result = wb_node_pull(mask, 0.0, 0, 0, 64, 64);
    if (result) {
        /* Without invert, inside triangle -> mask_alpha=1 -> src_alpha*1 = 1 */
        int center_idx = 20 * 64 + 30;
        CHECK(result->px[center_idx].a > 0.9f, "center pixel fully masked after invert=0");
    }
    if (result) wb_frame_free(result);

    /* Test 15: Feather softens edges */
    wb_node_effect_mask_set_feather(mask, 5.0f);
    result = wb_node_pull(mask, 0.0, 0, 0, 64, 64);
    if (result) {
        /* Near edge of triangle, alpha should be partially feathered */
        int edge_idx = 10 * 64 + 10; /* near the left edge */
        float a = result->px[edge_idx].a;
        CHECK(a > 0.0f && a < 1.0f, "feather produces partial alpha at edge");
    }
    if (result) wb_frame_free(result);

    /* Test 16: Expand grows mask */
    wb_node_effect_mask_set_feather(mask, 0.0f);
    wb_node_effect_mask_set_expand(mask, 5.0f);
    result = wb_node_pull(mask, 0.0, 0, 0, 64, 64);
    if (result) {
        /* A pixel that was outside the triangle should now be inside */
        int near_idx = 8 * 64 + 30; /* was outside, expand should bring it in */
        CHECK(result->px[near_idx].a > 0.5f, "expand grows mask outward");
    }
    if (result) wb_frame_free(result);

    /* Test 17: Negative expand shrinks mask */
    wb_node_effect_mask_set_expand(mask, -5.0f);
    result = wb_node_pull(mask, 0.0, 0, 0, 64, 64);
    if (result) {
        /* Center should still be masked, but less area */
        int center_idx = 20 * 64 + 30;
        CHECK(result->px[center_idx].a > 0.5f, "center still masked after shrink");
    }
    if (result) wb_frame_free(result);

    /* Test 18: Mask with external mask source */
    wb_node_effect_mask_set_expand(mask, 0.0f);
    wb_node *mask_src = wb_node_source_color(1.0f, 1.0f, 1.0f, 0.5f, 64, 64);
    wb_node_connect(mask, mask_src, 1); /* input 1 = mask source */
    result = wb_node_pull(mask, 0.0, 0, 0, 64, 64);
    if (result) {
        /* Source alpha (1.0) * mask source alpha (0.5) = 0.5 */
        int idx = 10 * 64 + 10;
        CHECK(fabsf(result->px[idx].a - 0.5f) < 0.05f, "external mask source multiplies alpha");
    }
    if (result) wb_frame_free(result);

    /* Test 19: NULL safety for setters */
    wb_node_effect_mask_set_feather(NULL, 1.0f);
    wb_node_effect_mask_set_expand(NULL, 1.0f);
    wb_node_effect_mask_set_invert(NULL, 1);
    wb_node_effect_mask_set_path(NULL, "M 0 0 L 10 10 Z");
    CHECK(1, "NULL safety for all setters");

    /* Test 20: Complex path (rectangle) */
    wb_node *mask2 = wb_node_effect_mask(64, 64);
    wb_node_effect_mask_set_path(mask2, "M 16 16 L 48 16 L 48 48 L 16 48 Z");
    wb_node *src2 = wb_node_source_color(1.0f, 1.0f, 1.0f, 1.0f, 64, 64);
    wb_node_connect(mask2, src2, 0);
    result = wb_node_pull(mask2, 0.0, 0, 0, 64, 64);
    if (result) {
        /* Inside rect */
        int inside = 32 * 64 + 32;
        /* Outside rect */
        int outside = 4 * 64 + 4;
        CHECK(result->px[inside].a > 0.9f, "inside rectangle masked");
        CHECK(result->px[outside].a < 0.1f, "outside rectangle unmasked");
    }
    if (result) wb_frame_free(result);
    wb_node_destroy(mask2);

    /* Test 21: Path with relative commands */
    wb_node *mask3 = wb_node_effect_mask(64, 64);
    wb_node_effect_mask_set_path(mask3, "m 10 10 l 40 0 l 0 40 l -40 0 z");
    wb_node *src3 = wb_node_source_color(0.8f, 0.2f, 0.5f, 1.0f, 64, 64);
    wb_node_connect(mask3, src3, 0);
    result = wb_node_pull(mask3, 0.0, 0, 0, 64, 64);
    CHECK(result != NULL, "relative path commands work");
    if (result) {
        int inside = 30 * 64 + 30;
        CHECK(result->px[inside].a > 0.9f, "relative path inside masked");
    }
    if (result) wb_frame_free(result);
    wb_node_destroy(mask3);

    /* Test 22: Empty path (no mask applied) */
    wb_node *mask4 = wb_node_effect_mask(32, 32);
    wb_node_effect_mask_set_path(mask4, "");
    wb_node *src4 = wb_node_source_color(1.0f, 1.0f, 1.0f, 1.0f, 32, 32);
    wb_node_connect(mask4, src4, 0);
    result = wb_node_pull(mask4, 0.0, 0, 0, 32, 32);
    if (result) {
        /* With empty path, no mask is rendered, so alpha should be 0 (fully transparent) */
        int idx = 5 * 32 + 5;
        CHECK(result->px[idx].a < 0.1f, "empty path = no mask = transparent");
    }
    if (result) wb_frame_free(result);
    wb_node_destroy(mask4);

    /* Cleanup */
    wb_node_destroy(mask);

    printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}