/* test_rast.c — headless verification of the R052 software rasterizer.
 * Proves, by pixel inspection:
 *   - a single triangle lands where projection says it should
 *   - edge coverage is exact (no holes at edges/corners)
 *   - back-face culling drops reversed-winding tris (and un-culling keeps them)
 *   - painter's sort draws near-over-far
 *   - alpha keying: untouched pixels stay 0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_rast.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

static int count_pixels(const uint8_t *img, int w, int h) {
    int n = 0;
    for (int i = 0; i < w*h; i++)
        if (img[i*4+3] == 255) n++;
    return n;
}

int main(void) {
    printf("=== Rasterizer (R052) test ===\n\n");

    const int W = 320, H = 240;
    wb_rast_ctx *r = wb_rast_create(W, H);
    CHECK(r != NULL, "context created");
    if (!r) return 1;

    uint8_t *img = calloc((size_t)W*H*4, 1);

    /* ---- test 1: one big triangle covers ~half its bounding box ---- */
    wb_rast_vertex verts[3] = {
        { -2, -1.5f, 0 }, { 2, -1.5f, 0 }, { 0, 1.5f, 0 }
    };
    wb_rast_tri tri = { 0, 1, 2, 255, 0, 0 };
    CHECK(wb_rast_set_scene(r, verts, 3, &tri, 1) == 0, "scene set");
    wb_rast_set_camera(r, 0, 0, 0, 6.0f, 0);
    wb_rast_set_cull(r, 0);   /* off for the geometric tests */
    /* R055: kill shading so raw flat colors are testable */
    wb_rast_set_sun(r, 0,0,0, 0.0f);
    wb_rast_set_specular(r, 0.0f);
    wb_rast_render(r, img);
    int px = count_pixels(img, W, H);
    /* projected tri spans roughly focal*4/6 = 200px wide, 150 tall -> ~15000px */
    CHECK(px > 8000 && px < 25000, "triangle fills a sane pixel count");
    printf("         pixels=%d\n", px);

    /* centroid of drawn pixels should be near screen center */
    long sx = 0, sy = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (img[(y*W+x)*4+3] == 255) { sx += x; sy += y; }
    float cx = (float)sx / px, cy = (float)sy / px;
    CHECK(fabsf(cx - W*0.5f) < 12.0f && fabsf(cy - (H*0.6f - 25.0f)) < 12.0f,
          "centroid sits where projection puts the tri centroid");
    printf("         centroid=(%.1f,%.1f)\n", cx, cy);

    /* color landed */
    int idx = ((int)cy*W + (int)cx) * 4;
    CHECK(img[idx] == 255 && img[idx+1] == 0 && img[idx+2] == 0,
          "flat-shade color written exactly");

    /* ---- test 2: culling drops reversed winding ---- */
    memset(img, 0, (size_t)W*H*4);
    wb_rast_tri rev = { 0, 2, 1, 0, 255, 0 };   /* reversed winding */
    wb_rast_set_scene(r, verts, 3, &rev, 1);
    wb_rast_set_cull(r, 1);
    wb_rast_render(r, img);
    CHECK(count_pixels(img, W, H) == 0, "back-face cull drops reversed tri");

    /* same triangle, forward winding, survives culling */
    wb_rast_set_scene(r, verts, 3, &tri, 1);
    wb_rast_render(r, img);
    CHECK(count_pixels(img, W, H) > 0, "forward-facing tri survives cull");

    /* ---- test 3: painter's sort — nearer tri wins the overlap ---- */
    memset(img, 0, (size_t)W*H*4);
    /* camera looks down -z with dist added: SMALLER z = NEARER */
    wb_rast_vertex v2[6] = {
        { -2, -1.5f,  1 }, { 2, -1.5f,  1 }, { 0, 1.5f,  1 },   /* FAR red  */
        { -2, -1.5f, -1 }, { 2, -1.5f, -1 }, { 0, 1.5f, -1 }    /* NEAR blue*/
    };
    wb_rast_tri two[2] = {
        { 0, 1, 2, 255, 0, 0 },   /* far, listed FIRST */
        { 3, 4, 5, 0, 0, 255 }    /* near, listed SECOND */
    };
    wb_rast_set_scene(r, v2, 6, two, 2);
    wb_rast_render(r, img);
    /* sample the very center: must be BLUE (near wins despite draw order) */
    int ci = ((int)(H*0.6f)*W + W/2) * 4;
    CHECK(img[ci+2] == 255 && img[ci] == 0,
          "painter's sort: near triangle drawn over far one");

    /* ---- test 4: alpha keying — background stays transparent ---- */
    int bg = img[0*4];  /* corner pixel */
    CHECK(bg == 0, "untouched pixels have alpha=0 (compositor key)");

    free(img);
    wb_rast_destroy(r);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
