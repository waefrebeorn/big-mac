/* test_composition.c — Composition Timeline System tests (R103) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(c, m) do { if (c) { p++; printf("  PASS: %s\n", m); } \
                         else { f++; printf("  FAIL: %s\n", m); } } while(0)

int main(void) {
    int p = 0, f = 0;

    printf("=== R103: Composition Timeline System ===\n\n");

    /* ---- Keyframe Track ---- */
    printf("--- Keyframe Track ---\n");
    wb_comp_kf_track kf;
    wb_comp_kf_init(&kf, 0.0f);
    wb_comp_kf_add(&kf, 0.0f, 0.0f, 1);
    wb_comp_kf_add(&kf, 1.0f, 10.0f, 1);
    wb_comp_kf_add(&kf, 2.0f, 5.0f, 1);
    CHECK(kf.n_keys == 3, "kf: 3 keyframes");
    CHECK(wb_comp_kf_eval(&kf, 0.0f) == 0.0f, "kf: eval at 0 = 0");
    CHECK(wb_comp_kf_eval(&kf, 1.0f) == 10.0f, "kf: eval at 1 = 10");
    CHECK(fabsf(wb_comp_kf_eval(&kf, 0.5f) - 5.0f) < 0.01f, "kf: linear interp");

    /* ---- 3D Transform ---- */
    printf("\n--- 3D Transform ---\n");
    wb_comp_transform_3d t;
    wb_comp_transform_init(&t);
    CHECK(t.sx == 1.0f, "transform: default scale=1");
    t.x = 100.0f; t.rz = M_PI / 4;
    float ox, oy;
    wb_comp_transform_apply(&t, 10.0f, 0.0f, &ox, &oy);
    CHECK(ox > 100.0f, "transform: translated X");

    /* ---- Track Matte ---- */
    printf("\n--- Track Matte ---\n");
    wb_comp_track_matte matte;
    wb_comp_matte_init(&matte);
    CHECK(matte.matte_layer == -1, "matte: no matte by default");
    matte.matte_layer = 1;
    matte.matte_type = WB_MATTE_ALPHA;
    float result = wb_comp_matte_apply(&matte, 0.8f, 0.5f, 0.0f);
    CHECK(fabsf(result - 0.4f) < 0.01f, "matte: alpha 0.8*0.5=0.4");

    /* ---- Composition ---- */
    printf("\n--- Composition ---\n");
    wb_comp_comp c;
    wb_comp_timeline_init(&c, 320, 240, 30.0f, 10.0f);
    fprintf(stderr, "INIT OK: w=%d h=%d out=%p\n", c.width, c.height, (void*)c.output);
    CHECK(c.width == 320, "comp: width=320");
    CHECK(c.output != NULL, "comp: output buffer allocated");

    int bg = wb_comp_timeline_add_track(&c, COMP_TRACK_VIDEO, "background");
    fprintf(stderr, "ADD TRACK OK: idx=%d n=%d\n", bg, c.n_tracks);
    CHECK(bg == 0, "comp: track index=0");
    CHECK(c.n_tracks == 1, "comp: 1 track");

    wb_comp_timeline_free(&c);
    CHECK(1, "comp: freed");

    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    wb_comp_kf_init(NULL, 0);
    wb_comp_kf_add(NULL, 0, 0, 0);
    wb_comp_kf_eval(NULL, 0);
    wb_comp_transform_init(NULL);
    wb_comp_transform_apply(NULL, 0, 0, NULL, NULL);
    wb_comp_matte_init(NULL);
    wb_comp_matte_apply(NULL, 0, 0, 0);
    wb_comp_timeline_init(NULL, 0, 0, 0, 0);
    wb_comp_timeline_free(NULL);
    wb_comp_timeline_add_track(NULL, 0, NULL);
    wb_comp_timeline_get_clip_at(NULL, 0, 0);
    wb_comp_timeline_render_frame(NULL, 0);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", p, p + f);
    return f > 0 ? 1 : 0;
}
