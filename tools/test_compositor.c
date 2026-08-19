/* test_compositor.c — headless verification of keyframe tracks + pull
 * node-graph compositor (R013 D1/D3, R016 S2). */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "wbus/wbus_param_track.h"
#include "wbus/wbus_compositor.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== Compositor + keyframe test (R013/R016) ===\n\n");

    /* ---- keyframe tracks ---- */
    printf("-- keyframe tracks --\n");
    wb_param_track *tr = wb_param_track_create();
    wb_param_track_set(tr, 0.0, 0.0f, WB_KF_LINEAR);
    wb_param_track_set(tr, 1.0, 1.0f, WB_KF_LINEAR);
    wb_param_track_set(tr, 2.0, 0.0f, WB_KF_LINEAR);
    CHECK(wb_param_track_count(tr) == 3, "3 keys set");
    CHECK(fabsf(wb_param_track_value_at(tr, 0.0) - 0.0f) < 1e-5f, "t=0 -> 0");
    CHECK(fabsf(wb_param_track_value_at(tr, 0.5) - 0.5f) < 1e-5f, "t=0.5 linear -> 0.5");
    CHECK(fabsf(wb_param_track_value_at(tr, 1.5) - 0.5f) < 1e-5f, "t=1.5 linear -> 0.5");
    /* valid-clamp: before first / after last */
    CHECK(fabsf(wb_param_track_value_at(tr, -1.0) - 0.0f) < 1e-5f, "clamp before first");
    CHECK(fabsf(wb_param_track_value_at(tr, 9.0) - 0.0f) < 1e-5f, "clamp after last");
    /* hold interp */
    wb_param_track_set(tr, 0.0, 0.0f, WB_KF_HOLD);
    wb_param_track_set(tr, 1.0, 1.0f, WB_KF_HOLD);
    CHECK(fabsf(wb_param_track_value_at(tr, 0.9) - 0.0f) < 1e-5f, "hold: 0.9 -> 0");
    CHECK(fabsf(wb_param_track_value_at(tr, 1.0) - 1.0f) < 1e-5f, "hold: 1.0 -> 1");
    /* bezier */
    wb_param_track *bt = wb_param_track_create();
    wb_param_track_set(bt, 0.0, 0.0f, WB_KF_BEZIER);
    wb_param_track_set_tangents(bt, 0.0, 0, 0.9f, 1, 1);
    wb_param_track_set(bt, 1.0, 1.0f, WB_KF_BEZIER);
    wb_param_track_set_tangents(bt, 1.0, -0.9f, 0, 1, 1);
    float bmid = wb_param_track_value_at(bt, 0.5);
    CHECK(bmid > 0.0f && bmid < 1.0f, "bezier midpoint in (0,1)");
    /* asymmetric tangents: +out on first key, flat on second -> ahead */
    wb_param_track *at2 = wb_param_track_create();
    wb_param_track_set(at2, 0.0, 0.0f, WB_KF_BEZIER);
    wb_param_track_set_tangents(at2, 0.0, 0, 0.9f, 1, 1);
    wb_param_track_set(at2, 1.0, 1.0f, WB_KF_BEZIER);
    wb_param_track_set_tangents(at2, 1.0, 0.0f, 0, 1, 1);
    float ahead = wb_param_track_value_at(at2, 0.5);
    CHECK(ahead > 0.5f, "asymmetric bezier (out=0.9) ahead at midpoint");
    CHECK(ahead < 0.95f, "asymmetric bezier still bounded");
    wb_param_track_free(at2);
    wb_param_track_free(tr); wb_param_track_free(bt);

    /* ---- compositor: source -> effect(gain) -> cache -> composite ---- */
    printf("\n-- node graph --\n");
    wb_node *red  = wb_node_source_color(1.0f, 0.0f, 0.0f, 1.0f, 64, 64);
    wb_node *blue = wb_node_source_color(0.0f, 0.0f, 1.0f, 1.0f, 64, 64);
    wb_node *gain = wb_node_effect(1, 0.5f);   /* red*0.5 */
    gain->inputs[0] = red;
    wb_node *cg = wb_node_cache(gain, 8);
    wb_node *comp = wb_node_composite();
    wb_composite_add(comp, cg);
    wb_composite_add(comp, blue);

    wb_frame *f = wb_node_pull(comp, 0.0, 0, 0, 64, 64);
    CHECK(f != NULL, "composite pull returned frame");
    if (f) {
        /* top layer (blue) should dominate center via alpha-over */
        wb_px center = f->px[32*f->w + 32];
        CHECK(center.b > 0.8f, "center is mostly blue (top layer wins)");
        CHECK(center.r < 0.3f, "red contribution reduced (gain 0.5 + under blue)");
        wb_frame_free(f);
    }

    /* identity short-circuit: op 0 effect passes through */
    wb_node *pass = wb_node_effect(0, 1.0f);
    pass->inputs[0] = red;
    wb_frame *f2 = wb_node_pull(pass, 0.0, 0, 0, 64, 64);
    CHECK(f2 != NULL, "identity effect pull ok");
    if (f2) {
        wb_px p = f2->px[10*64+10];
        CHECK(fabsf(p.r - 1.0f) < 1e-5f, "identity passes red unchanged");
        wb_frame_free(f2);
    }

    /* cache hit: pull same (t,roi) twice -> second returns identical copy */
    wb_node *cache_only = wb_node_cache(red, 4);
    wb_frame *a = wb_node_pull(cache_only, 1.23, 0, 0, 32, 32);
    wb_frame *b = wb_node_pull(cache_only, 1.23, 0, 0, 32, 32);
    CHECK(a && b, "cache returns frames on hits");
    if (a && b) {
        wb_px pa = a->px[5*a->w + 5], pb = b->px[5*b->w + 5];
        CHECK(fabsf(pa.r - pb.r) < 1e-6f, "cache hit identical to recompute");
        wb_frame_free(a); wb_frame_free(b);
    }

    wb_node_destroy(comp);
    wb_node_destroy(pass);
    wb_node_destroy(cache_only);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
