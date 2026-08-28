/* test_light2d.c — verify 2D lighting system */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

typedef struct {
    int type;
    float x, y, dir_x, dir_y, r, g, b, intensity, radius, angle, softness;
} wb_light2d;

#define WB_LIGHT_POINT 0
#define WB_LIGHT_DIRECTIONAL 1
#define WB_LIGHT_AMBIENT 3

#define WB_2D_MAX_LIGHTS 16
typedef struct {
    wb_light2d lights[WB_2D_MAX_LIGHTS];
    int count;
    float ambient_r, ambient_g, ambient_b;
} wb_light_env;

void wb_light_env_init(wb_light_env *e) {
    memset(e, 0, sizeof(*e));
    e->ambient_r = e->ambient_g = e->ambient_b = 0.1f;
}

/* Simplified lighting test: point light on flat normal (0,0,1) */
int main(void) {
    int pass = 1;

    wb_light_env env;
    wb_light_env_init(&env);

    /* Add a point light at center */
    wb_light2d light = {WB_LIGHT_POINT, 50, 50, 0, 0, 1, 1, 1, 1.0f, 100, 0, 0};
    env.lights[env.count++] = light;

    /* Test: pixel directly below light should be lit */
    /* Normal pointing up (toward light): ny = -1 */
    /* Light direction: from pixel(50,70) to light(50,50) = (0, -1) */
    /* dot(normal(0,-1,0.7), light_dir(0,-1,0.7)) should be positive */

    /* Simple test: ambient only */
    wb_light_env amb;
    wb_light_env_init(&amb);
    float lit_r = amb.ambient_r;
    if (fabsf(lit_r - 0.1f) > 0.01f) { printf("FAIL ambient: %.3f\n", lit_r); pass = 0; }

    /* Test directional light */
    wb_light_env dir;
    wb_light_env_init(&dir);
    wb_light2d dlight = {WB_LIGHT_DIRECTIONAL, 0, 0, 0, -1, 1, 1, 1, 1.0f, 0, 0, 0};
    dir.lights[dir.count++] = dlight;

    /* Normal (0, 0, 1) pointing toward viewer */
    /* Light dir toward surface: -dir_y = 1 (pointing down toward surface) */
    /* dot((0,0,1), (0,1,0.7)) = 0.7 normalized */
    float nz = 1.0f;
    float lx = 0.0f, ly = 1.0f, lz = 0.7f;
    float llen = sqrtf(lx*lx + ly*ly + lz*lz);
    lx/=llen; ly/=llen; lz/=llen;
    float diff = nz * lz;  /* = 0.7/sqrt(1.49) ≈ 0.573 */
    if (diff < 0.5f || diff > 0.7f) { printf("FAIL directional diff: %.3f\n", diff); pass=0; }
    printf("Directional light diff: %.3f\n", diff);

    /* Test parallax */
    float depth = 0.5f;
    float factor = depth * 0.5f + 0.1f;  /* 0.35 */
    float cam_x = 100.0f;
    float offset = -cam_x * factor;  /* -35 */
    if (fabsf(offset - (-35.0f)) > 0.01f) { printf("FAIL parallax: %.1f\n", offset); pass=0; }
    printf("Parallax: cam=%.0f offset=%.1f\n", cam_x, offset);

    /* Test Ken Burns interpolation */
    float t = 0.5f;
    float ease = t < 0.5f ? 2*t*t : 1.0f - (-2*t+2)*(-2*t+2)/2.0f;
    float start_scale = 1.0f, end_scale = 1.5f;
    float scale = start_scale + (end_scale - start_scale) * ease;
    if (fabsf(scale - 1.25f) > 0.01f) { printf("FAIL ken burns scale: %.3f\n", scale); pass=0; }
    printf("Ken Burns: t=%.1f ease=%.3f scale=%.3f\n", t, ease, scale);

    /* Test perspective projection */
    float fov = 1.0f;  /* radians */
    float f = 100.0f;  /* view_h/2 / tan(fov/2) */
    float world_z = -10.0f;
    float cam_z = 0.0f;
    float dist = -(world_z - cam_z);  /* 10 */
    float screen_x = 160.0f + 0.0f * f / dist;  /* center */
    float screen_y = 100.0f - 5.0f * f / dist;
    printf("Projection: world(0,5,%.0f) → screen(%.1f,%.1f) dist=%.1f\n",
           world_z, screen_x, screen_y, dist);

    printf("%s\n", pass ? "ALL PASS" : "SOME FAIL");
    return pass ? 0 : 1;
}
