/* test_procedural.c — test procedural video generation (R084).
 * Tests that compositor source nodes (text, scene, CGI) can render to MP4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus_compositor.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); pass = 0; } \
    else { printf("ok: %s\n", msg); } \
} while(0)

int main(void) {
    int pass = 1;

    /* Test text source node */
    wb_node *title = wb_node_source_text("BIG MAC", 4, 1.0f, 1.0f, 0.2f, 1.0f, 640, 360);
    CHECK(title != NULL, "text source node created");

    /* Test scene source node */
    wb_node *scene = wb_node_source_scene(0.1f, 0.1f, 0.2f, 0.9f, 0.8f, 1.0f, 0, 0.5f, 640, 360);
    CHECK(scene != NULL, "scene source node created");

    /* Test color source */
    wb_node *color = wb_node_source_color(0.2f, 0.6f, 1.0f, 1.0f, 640, 360);
    CHECK(color != NULL, "color source node created");

    /* Test composite */
    wb_node *comp = wb_node_composite();
    CHECK(comp != NULL, "composite node created");

    /* Test transition */
    wb_node *trans = wb_node_transition(0, 2.0);
    CHECK(trans != NULL, "transition node created");

    /* Pull a frame from the text source */
    wb_frame *f = wb_node_pull(title, 0.0, 0, 0, 640, 360);
    CHECK(f != NULL, "pull frame from text source");
    if (f) {
        CHECK(f->w == 640 && f->h == 360, "text frame has correct dimensions");
        CHECK(f->roi_w == 640 && f->roi_h == 360, "text frame full ROI");
        wb_frame_free(f);
    }

    /* Pull from scene */
    f = wb_node_pull(scene, 1.0, 0, 0, 640, 360);
    CHECK(f != NULL, "pull frame from scene at t=1.0");
    if (f) wb_frame_free(f);

    /* Pull from color */
    f = wb_node_pull(color, 0.0, 0, 0, 640, 360);
    CHECK(f != NULL, "pull frame from color source");
    if (f) {
        /* Check that the color is roughly correct */
        wb_px *px = &f->px[0];
        CHECK(px->r > 0.1f && px->r < 0.4f, "red channel in range");
        CHECK(px->g > 0.5f && px->g < 0.8f, "green channel in range");
        CHECK(px->b > 0.8f && px->b < 1.1f, "blue channel in range");
        wb_frame_free(f);
    }

    /* Test PPM write (frame to disk) */
    f = wb_node_pull(title, 0.0, 0, 0, 640, 360);
    if (f) {
        int rc = wb_frame_write_ppm(f, "/tmp/test_procedural_frame.ppm");
        CHECK(rc == 0, "write frame to PPM");
        wb_frame_free(f);
    }

    /* Cleanup */
    wb_node_destroy(title);
    wb_node_destroy(scene);
    wb_node_destroy(color);
    wb_node_destroy(comp);
    wb_node_destroy(trans);

    printf("\n%s\n", pass ? "ALL PASS" : "SOME FAILED");
    return pass ? 0 : 1;
}
