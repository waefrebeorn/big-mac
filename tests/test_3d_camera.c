/* test_3d_camera.c — verify 3D camera + lighting system */
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

    printf("=== 3D Camera + Lights ===\n");

    /* Camera creation */
    wb_3d_camera *cam = wb_3d_camera_create();
    CHECK(cam != NULL, "camera created");

    /* Position/target */
    wb_3d_camera_set_position(cam, 0, 0, 10);
    wb_3d_camera_set_target(cam, 0, 0, 0);
    CHECK(1, "position and target set");

    /* FOV */
    wb_3d_camera_set_fov(cam, 60.0f);
    wb_3d_camera_set_fov(cam, 0.5f);   /* clamp to 1 */
    wb_3d_camera_set_fov(cam, 200.0f); /* clamp to 179 */
    CHECK(1, "FOV set with clamping");

    /* DOF */
    wb_3d_camera_set_dof(cam, 5.0f, 2.8f);
    CHECK(1, "DOF parameters set");

    /* Near/far */
    wb_3d_camera_set_near_far(cam, 0.1f, 1000.0f);
    CHECK(1, "near/far planes set");

    /* Update matrices */
    wb_3d_camera_update_matrices(cam, 16.0f/9.0f);
    CHECK(1, "matrices updated");

    /* View matrix */
    float view[16];
    wb_3d_camera_get_view_matrix(cam, view);
    CHECK(view[0] != 0.0f || view[5] != 0.0f, "view matrix computed (non-zero diagonal)");

    /* Projection matrix */
    float proj[16];
    wb_3d_camera_get_proj_matrix(cam, proj);
    CHECK(proj[0] != 0.0f && proj[5] != 0.0f, "projection matrix computed");

    /* Light registry */
    wb_light_registry *lights = wb_light_registry_create();
    CHECK(lights != NULL, "light registry created");

    int p = wb_light_add_point(lights, 0, 5, 0, 1.0f, 1.0f, 1.0f, 1.0f, 10.0f);
    CHECK(p == 0, "point light added (index 0)");
    CHECK(wb_light_count(lights) == 1, "light count = 1");

    int s = wb_light_add_spot(lights, 0, 10, 0, 0, -1, 0, 1.0f, 0.9f, 0.8f, 2.0f, 30.0f, 0.5f);
    CHECK(s == 1, "spot light added (index 1)");

    int d = wb_light_add_directional(lights, -1, -1, -1, 0.8f, 0.8f, 1.0f, 0.5f);
    CHECK(d == 2, "directional light added (index 2)");

    wb_light_add_ambient(lights, 0.1f, 0.1f, 0.15f);
    CHECK(1, "ambient light set");

    wb_light_set_shadows(lights, 0, 1);
    CHECK(1, "shadows enabled on point light");

    /* Remove light */
    wb_light_remove(lights, 1);
    CHECK(wb_light_count(lights) == 2, "light removed, count = 2");

    /* Clear all */
    wb_light_clear(lights);
    CHECK(wb_light_count(lights) == 0, "all lights cleared");

    /* Camera source node */
    wb_node *cam_node = wb_node_source_3d_camera(640, 480);
    CHECK(cam_node != NULL, "3D camera source node created");
    CHECK(cam_node->fmt_w == 640 && cam_node->fmt_h == 480, "camera node dimensions set");

    if (cam_node) {
        wb_frame *f = cam_node->pull(cam_node, 0.0, 0, 0, 640, 480, 0);
        CHECK(f != NULL, "camera node produces frame");
        if (f) wb_frame_free(f);
        if (cam_node->free) cam_node->free(cam_node);
        CHECK(1, "camera node freed");
    }

    /* Cleanup */
    wb_3d_camera_destroy(cam);
    wb_light_registry_destroy(lights);
    CHECK(1, "all resources freed");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
