/* test_matte_fx.c — Matte Painting + Cutout System tests (R108) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(cond, msg) do { \
    if (cond) { passed++; } \
    else { failed++; fprintf(stderr, "  FAIL: %s\n", msg); } \
} while(0)

int main(void) {
    int passed = 0, failed = 0;
    
    fprintf(stderr, "=== R108: Matte Painting + Cutout ===\n");
    
    /* --- Rotoscope tests --- */
    fprintf(stderr, "\n-- Rotoscope --\n");
    
    wb_rotoscope r;
    wb_rotoscope_init(&r, 64, 48);
    CHECK(r.width == 64 && r.height == 48, "rotoscope: init dimensions");
    CHECK(r.background != NULL, "rotoscope: background allocated");
    CHECK(r.threshold == 30, "rotoscope: default threshold");
    CHECK(r.feather == 2, "rotoscope: default feather");
    
    /* Create foreground frame (white) and background (black) */
    uint8_t *fg = (uint8_t *)calloc(64 * 48 * 4, 1);
    uint8_t *alpha = (uint8_t *)calloc(64 * 48 * 4, 1);
    
    /* Set foreground to white */
    for (int i = 0; i < 64 * 48; i++) {
        fg[i*4] = fg[i*4+1] = fg[i*4+2] = 255;
        fg[i*4+3] = 255;
    }
    
    /* Background is already black (calloc) */
    wb_rotoscope_apply(&r, fg, alpha);
    
    /* Center pixel should be foreground (white alpha) since fg differs from bg */
    int center_alpha = alpha[(24 * 64 + 32) * 4 + 3];
    CHECK(center_alpha > 200, "rotoscope: foreground detected (white on black)");
    
    /* Set foreground = background → alpha should be 0 */
    memset(fg, 0, 64 * 48 * 4);
    wb_rotoscope_apply(&r, fg, alpha);
    center_alpha = alpha[(24 * 64 + 32) * 4 + 3];
    CHECK(center_alpha < 10, "rotoscope: no difference = transparent");
    
    /* NULL safety */
    wb_rotoscope_apply(NULL, fg, alpha);
    wb_rotoscope_apply(&r, NULL, alpha);
    wb_rotoscope_apply(&r, fg, NULL);
    CHECK(1, "rotoscope: NULL safety");
    
    free(fg);
    free(alpha);
    wb_rotoscope_free(&r);
    CHECK(1, "rotoscope: free completes (no crash)");
    
    /* --- Depth matte tests --- */
    fprintf(stderr, "\n-- Depth Matte --\n");
    
    wb_depth_matte d;
    wb_depth_init(&d, 64, 48);
    CHECK(d.width == 64 && d.height == 48, "depth: init dimensions");
    CHECK(d.depth_type == DEPTH_LUMA, "depth: default type luma");
    CHECK(d.near_plane != d.far_plane, "depth: near != far plane (init worked)");
    
    uint8_t *frame = (uint8_t *)calloc(64 * 48 * 4, 1);
    uint8_t *depth = (uint8_t *)calloc(64 * 48 * 4, 1);
    
    /* Create gradient frame: top dark, bottom bright */
    for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 64; x++) {
            uint8_t val = (uint8_t)(y * 255 / 48);
            frame[(y * 64 + x) * 4] = val;
            frame[(y * 64 + x) * 4 + 1] = val;
            frame[(y * 64 + x) * 4 + 2] = val;
            frame[(y * 64 + x) * 4 + 3] = 255;
        }
    }
    
    wb_depth_generate(&d, frame, depth);
    
    /* Bottom row (bright) should have high depth value */
    int top_depth = depth[(2 * 64 + 32) * 4];     /* near top = dark */
    int bot_depth = depth[(46 * 64 + 32) * 4];    /* near bottom = bright */
    CHECK(bot_depth > top_depth, "depth: luma-based (bright=closer)");
    
    /* Test gradient type */
    d.depth_type = DEPTH_GRADIENT;
    wb_depth_generate(&d, frame, depth);
    top_depth = depth[(2 * 64 + 32) * 4];
    bot_depth = depth[(46 * 64 + 32) * 4];
    CHECK(bot_depth > top_depth, "depth: gradient (bottom=closer)");
    
    /* Test radial type */
    d.depth_type = DEPTH_RADIAL;
    wb_depth_generate(&d, frame, depth);
    int center_depth = depth[(24 * 64 + 32) * 4];
    int corner_depth = depth[(1 * 64 + 1) * 4];
    CHECK(center_depth > corner_depth, "depth: radial (center=closer)");
    
    /* NULL safety */
    wb_depth_generate(NULL, frame, depth);
    wb_depth_generate(&d, NULL, depth);
    wb_depth_generate(&d, frame, NULL);
    CHECK(1, "depth: NULL safety");
    
    free(frame);
    free(depth);
    
    /* --- 3D Parallax tests --- */
    fprintf(stderr, "\n-- 3D Parallax --\n");
    
    wb_parallax_3d p;
    wb_parallax_init(&p, 64, 48);
    CHECK(p.width == 64 && p.height == 48, "parallax: init dimensions");
    CHECK(p.n_layers == 3, "parallax: default 3 layers");
    CHECK(p.camera_z == 5.0f, "parallax: default camera z");
    CHECK(p.fov > 0.0f, "parallax: fov set");
    
    uint8_t *src = (uint8_t *)calloc(64 * 48 * 4, 1);
    uint8_t *dst = (uint8_t *)calloc(64 * 48 * 4, 1);
    
    /* Create a test pattern: red cross in center area */
    for (int y = 20; y < 28; y++) {
        for (int x = 28; x < 36; x++) {
            src[(y * 64 + x) * 4] = 255;
            src[(y * 64 + x) * 4 + 3] = 255;
        }
    }
    
    wb_parallax_apply_layer(&p, src, dst, 0.5f);
    
    /* Output should have some non-zero pixels (transform applied) */
    int nonzero = 0;
    for (int i = 0; i < 64 * 48; i++) {
        if (dst[i*4] > 0) nonzero++;
    }
    CHECK(nonzero > 0, "parallax: transform produces output");
    
    /* Move camera slightly → parallax should shift layer */
    p.camera_x = 0.5f;
    memset(dst, 0, 64 * 48 * 4);
    wb_parallax_apply_layer(&p, src, dst, 0.5f);
    int shifted = 0;
    for (int i = 0; i < 64 * 48; i++) {
        if (dst[i*4] > 0) shifted++;
    }
    CHECK(shifted > 0, "parallax: camera move produces output");
    
    /* NULL safety */
    wb_parallax_apply_layer(NULL, src, dst, 0.5f);
    wb_parallax_apply_layer(&p, NULL, dst, 0.5f);
    wb_parallax_apply_layer(&p, src, NULL, 0.5f);
    CHECK(1, "parallax: NULL safety");
    
    free(src);
    free(dst);
    
    /* --- Summary --- */
    fprintf(stderr, "\n=== R108 Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
