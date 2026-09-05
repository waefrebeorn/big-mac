/* tests/test_ytpmv_composite.c — YTPMV compositing engine tests. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* Forward declarations (opaque C11 style — no god headers) */
typedef enum {
    WB_COMP_BLEND_OVER = 0,
    WB_COMP_BLEND_ADD,
    WB_COMP_BLEND_MULTIPLY,
    WB_COMP_BLEND_SCREEN,
    WB_COMP_BLEND_OVERLAY,
    WB_COMP_BLEND_NORMAL = WB_COMP_BLEND_OVER
} wb_comp_blend_mode;

typedef enum {
    WB_LAYER_VIDEO = 0,
    WB_LAYER_IMAGE,
    WB_LAYER_COLOR
} wb_layer_type;

/* From meta-layer enum in wbus_compositor.h */
#define WB_LAYER_SOLID_COLOR 17

typedef struct {
    char name[64];
    char source_path[256];
    int type;
    int x, y;
    int width, height;
    float opacity;
    wb_comp_blend_mode blend;
    int visible;
    int use_chromakey;
    float key_r, key_g, key_b;
    float key_threshold;
    float key_softness;
} wb_comp_layer;

typedef struct {
    wb_comp_layer layers[16];
    int n_layers;
    int width, height;
    char filter_str[4096];
    char bg_color[32];
} wb_composite_ctx;

wb_composite_ctx *wb_composite_create(int w, int h);
void wb_composite_destroy(wb_composite_ctx *ctx);
int wb_composite_add_layer(wb_composite_ctx *ctx, const char *name, wb_layer_type type);
int wb_composite_add_video_layer(wb_composite_ctx *ctx, const char *name, const char *path);
int wb_composite_add_color_layer(wb_composite_ctx *ctx, const char *name, float r, float g, float b, float a);
void wb_layer_set_position(wb_composite_ctx *ctx, int idx, int x, int y);
void wb_layer_set_size(wb_composite_ctx *ctx, int idx, int w, int h);
void wb_layer_set_opacity(wb_composite_ctx *ctx, int idx, float opacity);
void wb_layer_set_blend(wb_composite_ctx *ctx, int idx, wb_comp_blend_mode mode);
void wb_layer_set_visible(wb_composite_ctx *ctx, int idx, int visible);
void wb_layer_set_chromakey(wb_composite_ctx *ctx, int idx, float r, float g, float b, float threshold, float softness);
int wb_composite_generate_filter(wb_composite_ctx *ctx);
int wb_composite_pip(wb_composite_ctx *ctx, int main_idx, int pip_idx, int pip_x, int pip_y, int pip_w, int pip_h);
int wb_composite_split_screen(wb_composite_ctx *ctx, int left_idx, int right_idx, int vertical);
const char *wb_composite_get_filter(const wb_composite_ctx *ctx);
int wb_composite_get_layer_count(const wb_composite_ctx *ctx);
const wb_comp_layer *wb_composite_get_layer(const wb_composite_ctx *ctx, int idx);
int wb_composite_generate_cmd(const wb_composite_ctx *ctx, char *buf, int max_len);

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    printf("=== YTPMV Compositing Engine Tests ===\n\n");

    /* Test 1: Create context */
    wb_composite_ctx *ctx = wb_composite_create(1920, 1080);
    CHECK(ctx != NULL);

    /* Test 2: Dimensions stored correctly */
    CHECK(ctx->width == 1920);
    CHECK(ctx->height == 1080);

    /* Test 3: Add video layer */
    int bg = wb_composite_add_video_layer(ctx, "background", "bg.mp4");
    CHECK(bg == 0);

    /* Test 4: Add overlay layer */
    int overlay = wb_composite_add_video_layer(ctx, "overlay", "overlay.mp4");
    CHECK(overlay == 1);

    /* Test 5: Layer count */
    CHECK(wb_composite_get_layer_count(ctx) == 2);

    /* Test 6: Set position */
    wb_layer_set_position(ctx, overlay, 100, 50);
    const wb_comp_layer *l = wb_composite_get_layer(ctx, overlay);
    CHECK(l != NULL);
    CHECK(l->x == 100);
    CHECK(l->y == 50);

    /* Test 7: Set size */
    wb_layer_set_size(ctx, overlay, 640, 480);
    CHECK(l->width == 640);
    CHECK(l->height == 480);

    /* Test 8: Set opacity */
    wb_layer_set_opacity(ctx, overlay, 0.75f);
    CHECK(l->opacity > 0.74f && l->opacity < 0.76f);

    /* Test 9: Set blend mode */
    wb_layer_set_blend(ctx, overlay, WB_COMP_BLEND_ADD);
    CHECK(l->blend == WB_COMP_BLEND_ADD);

    /* Test 10: Chroma key setup */
    wb_layer_set_chromakey(ctx, overlay, 0.0f, 1.0f, 0.0f, 0.3f, 0.1f);
    CHECK(l->use_chromakey == 1);
    CHECK(l->key_g > 0.99f);
    CHECK(l->key_threshold > 0.29f && l->key_threshold < 0.31f);

    /* Test 11: Generate filter string */
    int rc = wb_composite_generate_filter(ctx);
    CHECK(rc == 0);
    const char *filter = wb_composite_get_filter(ctx);
    CHECK(filter != NULL);
    CHECK(strlen(filter) > 0);
    printf("  Filter: %s\n", filter);

    /* Test 12: Picture-in-picture */
    int pip = wb_composite_add_video_layer(ctx, "pip", "pip.mp4");
    CHECK(pip == 2);
    rc = wb_composite_pip(ctx, bg, pip, 1500, 800, 320, 240);
    CHECK(rc == 0);
    const wb_comp_layer *pip_layer = wb_composite_get_layer(ctx, pip);
    CHECK(pip_layer->x == 1500);
    CHECK(pip_layer->y == 800);
    CHECK(pip_layer->width == 320);
    CHECK(pip_layer->height == 240);

    /* Test 13: Split screen */
    wb_composite_ctx *ctx2 = wb_composite_create(1920, 1080);
    int left = wb_composite_add_video_layer(ctx2, "left", "left.mp4");
    int right = wb_composite_add_video_layer(ctx2, "right", "right.mp4");
    rc = wb_composite_split_screen(ctx2, left, right, 1); /* vertical */
    CHECK(rc == 0);
    const wb_comp_layer *left_layer = wb_composite_get_layer(ctx2, left);
    const wb_comp_layer *right_layer = wb_composite_get_layer(ctx2, right);
    CHECK(left_layer->width == 960);
    CHECK(right_layer->x == 960);

    /* Test 14: Color layer */
    int color = wb_composite_add_color_layer(ctx2, "solid", 1.0f, 0.0f, 0.0f, 1.0f);
    CHECK(color >= 0);
    const wb_comp_layer *cl = wb_composite_get_layer(ctx2, color);
    CHECK(cl->type == WB_LAYER_SOLID_COLOR);
    CHECK(strstr(cl->source_path, "color:") != NULL);

    /* Test 15: Visibility toggle */
    wb_layer_set_visible(ctx2, left, 0);
    CHECK(left_layer->visible == 0);

    /* Test 16: Generate command string */
    char cmd[8192];
    rc = wb_composite_generate_cmd(ctx2, cmd, sizeof(cmd));
    CHECK(rc > 0);
    CHECK(strstr(cmd, "ffmpeg") != NULL);
    CHECK(strstr(cmd, "filter_complex") != NULL);

    /* Test 17: Opacity clamping (above 1.0) */
    wb_layer_set_opacity(ctx, overlay, 2.0f);
    CHECK(l->opacity <= 1.0f);

    /* Test 18: Opacity clamping (below 0.0) */
    wb_layer_set_opacity(ctx, overlay, -1.0f);
    CHECK(l->opacity >= 0.0f);

    /* Cleanup */
    wb_composite_destroy(ctx);
    wb_composite_destroy(ctx2);

    printf("\n=== Results: %d checks, %d failures ===\n", checks, fails);
    return fails > 0 ? 1 : 0;
}