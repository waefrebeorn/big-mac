/* test_3d_edit.c — test gate for wb_3d_edit.c
 *
 * Validates:
 *   - wb_node_source_stereo3d (source creation, pull, mono fallback)
 *   - wb_3d_mode_t enum values
 *   - wb_node_effect_3d_depth (creation, parameter setting, shift apply)
 *   - wb_node_composite_3d (all four output modes, dimension calculation)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ================================================================ */
/* Declarations from wb_3d_edit.c                                    */
/* ================================================================ */

typedef enum {
    WB_3D_ANAGLYPH = 0,
    WB_3D_SIDE_BY_SIDE,
    WB_3D_TOP_BOTTOM,
    WB_3D_CHECKERBOARD
} wb_3d_mode_t;

typedef struct wb_source_stereo3d wb_source_stereo3d_t;
typedef struct wb_effect_3d_depth wb_effect_3d_depth_t;
typedef struct wb_composite_3d wb_composite_3d_t;

wb_source_stereo3d_t *wb_node_source_stereo3d_create(const char *left_path,
                                                      const char *right_path,
                                                      int width, int height);
void wb_node_source_stereo3d_destroy(wb_source_stereo3d_t *s);
int wb_node_source_stereo3d_width(const wb_source_stereo3d_t *s);
int wb_node_source_stereo3d_height(const wb_source_stereo3d_t *s);
int wb_node_source_stereo3d_active(const wb_source_stereo3d_t *s);
int wb_node_source_stereo3d_pull(wb_source_stereo3d_t *s,
                                  uint8_t *left_buf,
                                  uint8_t *right_buf,
                                  int width, int height);

wb_effect_3d_depth_t *wb_node_effect_3d_depth_create(int width, int height);
void wb_node_effect_3d_depth_destroy(wb_effect_3d_depth_t *d);
void wb_node_effect_3d_depth_set(wb_effect_3d_depth_t *d, float depth);
void wb_node_effect_3d_depth_set_convergence(wb_effect_3d_depth_t *d, float px);
float wb_node_effect_3d_depth_get(const wb_effect_3d_depth_t *d);
void wb_node_effect_3d_depth_apply(const wb_effect_3d_depth_t *d,
                                    const uint8_t *left_eye,
                                    const uint8_t *right_eye,
                                    uint8_t *left_out,
                                    uint8_t *right_out,
                                    int width, int height);

wb_composite_3d_t *wb_node_composite_3d_create(wb_3d_mode_t mode);
void wb_node_composite_3d_destroy(wb_composite_3d_t *c);
void wb_node_composite_3d_set_mode(wb_composite_3d_t *c, wb_3d_mode_t mode);
void wb_node_composite_3d_set_tile_size(wb_composite_3d_t *c, int size);
wb_3d_mode_t wb_node_composite_3d_get_mode(const wb_composite_3d_t *c);
void wb_node_composite_3d_output_size(const wb_composite_3d_t *c,
                                       int eye_w, int eye_h,
                                       int *out_w, int *out_h);
int wb_node_composite_3d_apply(const wb_composite_3d_t *c,
                                const uint8_t *left_eye,
                                const uint8_t *right_eye,
                                uint8_t *out_buf,
                                int eye_w, int eye_h);

/* ================================================================ */
/* Test helpers                                                      */
/* ================================================================ */

static int count_passed = 0;
static int count_total  = 0;

#define TEST(name) do { count_total++; printf("  %d. %s: ", count_total, name); } while(0)
#define PASS() do { count_passed++; printf("PASS\n"); } while(0)

/* Fill buffer with a known pattern: R = x, G = y, B = (x+y), A = 255 */
static void fill_pattern(uint8_t *buf, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int p = (y * w + x) * 4;
            buf[p+0] = (uint8_t)(x & 0xFF);
            buf[p+1] = (uint8_t)(y & 0xFF);
            buf[p+2] = (uint8_t)((x + y) & 0xFF);
            buf[p+3] = 255;
        }
    }
}

/* ================================================================ */
/* Main test suite                                                   */
/* ================================================================ */

int main(void) {
    int w = 16, h = 16;
    int eye_size = w * h * 4;

    uint8_t *left  = malloc(eye_size);
    uint8_t *right = malloc(eye_size);
    uint8_t *out   = malloc(eye_size * 4);  /* max 2x2 = 4x */

    assert(left && right && out);

    /* ---- Source stereo3d tests ---- */

    TEST("Source create with valid paths");
    wb_source_stereo3d_t *src = wb_node_source_stereo3d_create(
        "/tmp/left.mp4", "/tmp/right.mp4", w, h);
    assert(src);
    PASS();

    TEST("Source dimensions correct");
    assert(wb_node_source_stereo3d_width(src) == w);
    assert(wb_node_source_stereo3d_height(src) == h);
    PASS();

    TEST("Source active flag set");
    assert(wb_node_source_stereo3d_active(src) == 1);
    PASS();

    TEST("Source pull fills both buffers");
    memset(left, 0, eye_size);
    memset(right, 0, eye_size);
    int rc = wb_node_source_stereo3d_pull(src, left, right, w, h);
    assert(rc == 0);
    /* Verify non-zero data */
    int nonzero = 0;
    for (int i = 0; i < eye_size; i++) {
        if (left[i] || right[i]) { nonzero = 1; break; }
    }
    assert(nonzero);
    PASS();

    TEST("Source pull rejects wrong dimensions");
    rc = wb_node_source_stereo3d_pull(src, left, right, w + 1, h);
    assert(rc == -1);
    PASS();

    TEST("Source mono fallback (NULL right path)");
    wb_source_stereo3d_t *src_mono = wb_node_source_stereo3d_create(
        "/tmp/only.mp4", NULL, w, h);
    assert(src_mono);
    assert(wb_node_source_stereo3d_active(src_mono) == 1);
    PASS();

    TEST("Source create rejects NULL left path");
    assert(wb_node_source_stereo3d_create(NULL, NULL, w, h) == NULL);
    PASS();

    TEST("Source create rejects zero dimensions");
    assert(wb_node_source_stereo3d_create("/tmp/a.mp4", NULL, 0, h) == NULL);
    PASS();

    /* ---- Enum tests ---- */

    TEST("Enum WB_3D_ANAGLYPH == 0");
    assert(WB_3D_ANAGLYPH == 0);
    PASS();

    TEST("Enum WB_3D_SIDE_BY_SIDE == 1");
    assert(WB_3D_SIDE_BY_SIDE == 1);
    PASS();

    TEST("Enum WB_3D_TOP_BOTTOM == 2");
    assert(WB_3D_TOP_BOTTOM == 2);
    PASS();

    TEST("Enum WB_3D_CHECKERBOARD == 3");
    assert(WB_3D_CHECKERBOARD == 3);
    PASS();

    /* ---- Depth effect tests ---- */

    TEST("Depth effect create");
    wb_effect_3d_depth_t *depth = wb_node_effect_3d_depth_create(w, h);
    assert(depth);
    PASS();

    TEST("Depth default is 0.0");
    assert(wb_node_effect_3d_depth_get(depth) == 0.0f);
    PASS();

    TEST("Depth set clamps to [-1, 1]");
    wb_node_effect_3d_depth_set(depth, 2.0f);
    assert(wb_node_effect_3d_depth_get(depth) <= 1.0f);
    wb_node_effect_3d_depth_set(depth, -2.0f);
    assert(wb_node_effect_3d_depth_get(depth) >= -1.0f);
    PASS();

    TEST("Depth apply produces shifted output");
    fill_pattern(left, w, h);
    fill_pattern(right, w, h);
    uint8_t *left_out  = malloc(eye_size);
    uint8_t *right_out = malloc(eye_size);
    assert(left_out && right_out);

    wb_node_effect_3d_depth_set(depth, 0.5f);
    wb_node_effect_3d_depth_set_convergence(depth, 0.0f);
    wb_node_effect_3d_depth_apply(depth, left, right, left_out, right_out, w, h);

    /* With positive depth, right eye should be shifted more to the right.
     * Verify output differs from input (shift occurred). */
    int differs = 0;
    for (int i = 0; i < eye_size; i += 4) {
        if (right_out[i] != right[i]) { differs = 1; break; }
    }
    assert(differs);
    PASS();

    TEST("Depth apply rejects NULL buffers");
    wb_node_effect_3d_depth_apply(depth, NULL, right, left_out, right_out, w, h);
    /* Should not crash — function returns early on NULL */
    PASS();

    TEST("Depth apply rejects wrong dimensions");
    uint8_t small_buf[1] = {0};
    wb_node_effect_3d_depth_apply(depth, left, right, small_buf, right_out, 1, 1);
    /* Should not crash — dimension mismatch returns early */
    PASS();

    /* ---- Composite 3D tests ---- */

    TEST("Composite create anaglyph");
    wb_composite_3d_t *comp = wb_node_composite_3d_create(WB_3D_ANAGLYPH);
    assert(comp);
    assert(wb_node_composite_3d_get_mode(comp) == WB_3D_ANAGLYPH);
    PASS();

    TEST("Composite set mode");
    wb_node_composite_3d_set_mode(comp, WB_3D_SIDE_BY_SIDE);
    assert(wb_node_composite_3d_get_mode(comp) == WB_3D_SIDE_BY_SIDE);
    PASS();

    TEST("Composite output size anaglyph = eye size");
    int ow, oh;
    wb_node_composite_3d_set_mode(comp, WB_3D_ANAGLYPH);
    wb_node_composite_3d_output_size(comp, w, h, &ow, &oh);
    assert(ow == w && oh == h);
    PASS();

    TEST("Composite output size side-by-side = 2w x h");
    wb_node_composite_3d_set_mode(comp, WB_3D_SIDE_BY_SIDE);
    wb_node_composite_3d_output_size(comp, w, h, &ow, &oh);
    assert(ow == w * 2 && oh == h);
    PASS();

    TEST("Composite output size top-bottom = w x 2h");
    wb_node_composite_3d_set_mode(comp, WB_3D_TOP_BOTTOM);
    wb_node_composite_3d_output_size(comp, w, h, &ow, &oh);
    assert(ow == w && oh == h * 2);
    PASS();

    TEST("Composite output size checkerboard = eye size");
    wb_node_composite_3d_set_mode(comp, WB_3D_CHECKERBOARD);
    wb_node_composite_3d_output_size(comp, w, h, &ow, &oh);
    assert(ow == w && oh == h);
    PASS();

    TEST("Composite anaglyph: R from left, GB from right");
    fill_pattern(left, w, h);
    fill_pattern(right, w, h);
    memset(out, 0, eye_size);
    wb_node_composite_3d_set_mode(comp, WB_3D_ANAGLYPH);
    rc = wb_node_composite_3d_apply(comp, left, right, out, w, h);
    assert(rc == 0);
    /* First pixel: R should come from left, G and B from right */
    assert(out[0] == left[0]);    /* R from left */
    assert(out[1] == right[1]);   /* G from right */
    assert(out[2] == right[2]);   /* B from right */
    assert(out[3] == 255);        /* A opaque */
    PASS();

    TEST("Composite side-by-side: left in left half, right in right half");
    memset(out, 0, (size_t)w * 2 * h * 4);
    wb_node_composite_3d_set_mode(comp, WB_3D_SIDE_BY_SIDE);
    rc = wb_node_composite_3d_apply(comp, left, right, out, w, h);
    assert(rc == 0);
    /* Pixel at (0,0) in output = left eye pixel (0,0) */
    assert(out[0] == left[0]);
    /* Pixel at (w,0) in output = right eye pixel (0,0) */
    int right_half_offset = w * 4;
    assert(out[right_half_offset] == right[0]);
    PASS();

    TEST("Composite top-bottom: left in top, right in bottom");
    memset(out, 0, (size_t)w * h * 2 * 4);
    wb_node_composite_3d_set_mode(comp, WB_3D_TOP_BOTTOM);
    rc = wb_node_composite_3d_apply(comp, left, right, out, w, h);
    assert(rc == 0);
    /* Pixel at (0,0) = left eye */
    assert(out[0] == left[0]);
    /* Pixel at (0,h) = right eye */
    int bottom_offset = h * w * 4;
    assert(out[bottom_offset] == right[0]);
    PASS();

    TEST("Composite checkerboard: tiles alternate left/right");
    memset(out, 0, eye_size);
    wb_node_composite_3d_set_mode(comp, WB_3D_CHECKERBOARD);
    wb_node_composite_3d_set_tile_size(comp, 8);
    rc = wb_node_composite_3d_apply(comp, left, right, out, w, h);
    assert(rc == 0);
    /* Tile (0,0) is even → left eye. Pixel (0,0) should be from left. */
    assert(out[0] == left[0]);
    /* Tile (1,0) is odd → right eye. Pixel (8,0) should be from right. */
    int tile1_offset = 8 * 4;
    assert(out[tile1_offset] == right[tile1_offset]);
    PASS();

    TEST("Composite rejects NULL buffers");
    rc = wb_node_composite_3d_apply(comp, NULL, right, out, w, h);
    assert(rc == -1);
    PASS();

    TEST("Composite rejects zero dimensions");
    rc = wb_node_composite_3d_apply(comp, left, right, out, 0, 0);
    assert(rc == -1);
    PASS();

    /* ---- Full pipeline test ---- */

    TEST("Full pipeline: source → depth → composite (anaglyph)");
    /* Reset source to known state */
    wb_node_source_stereo3d_pull(src, left, right, w, h);

    /* Apply depth effect */
    wb_node_effect_3d_depth_set(depth, 0.3f);
    wb_node_effect_3d_depth_set_convergence(depth, 1.0f);
    wb_node_effect_3d_depth_apply(depth, left, right, left_out, right_out, w, h);

    /* Composite to anaglyph */
    memset(out, 0, eye_size);
    wb_node_composite_3d_set_mode(comp, WB_3D_ANAGLYPH);
    rc = wb_node_composite_3d_apply(comp, left_out, right_out, out, w, h);
    assert(rc == 0);

    /* Verify anaglyph property: R from left_out, GB from right_out */
    assert(out[0] == left_out[0]);
    assert(out[1] == right_out[1]);
    assert(out[2] == right_out[2]);
    assert(out[3] == 255);
    PASS();

    TEST("Full pipeline: source → depth → composite (side-by-side)");
    memset(out, 0, (size_t)w * 2 * h * 4);
    wb_node_composite_3d_set_mode(comp, WB_3D_SIDE_BY_SIDE);
    rc = wb_node_composite_3d_apply(comp, left_out, right_out, out, w, h);
    assert(rc == 0);
    /* Left half starts with left_out pixel */
    assert(out[0] == left_out[0]);
    /* Right half starts with right_out pixel */
    assert(out[w * 4] == right_out[0]);
    PASS();

    /* ---- Cleanup ---- */

    free(left);
    free(right);
    free(out);
    free(left_out);
    free(right_out);
    wb_node_source_stereo3d_destroy(src);
    wb_node_source_stereo3d_destroy(src_mono);
    wb_node_effect_3d_depth_destroy(depth);
    wb_node_composite_3d_destroy(comp);

    printf("\nStereo 3D Edit: %d/%d passed\n", count_passed, count_total);
    return (count_passed == count_total) ? 0 : 1;
}