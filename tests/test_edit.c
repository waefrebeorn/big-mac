/* test_edit.c — test the video edit graph (R084) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_edit.h"
#include "wbus/wbus_compositor.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); pass = 0; } \
    else { printf("ok: %s\n", msg); } \
} while(0)

int main(void) {
    int pass = 1;

    /* Test edit graph creation */
    wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
    CHECK(g != NULL, "edit graph created");
    CHECK(g->fps == 30.0, "fps set correctly");
    CHECK(g->width == 854, "width set correctly");
    CHECK(g->height == 480, "height set correctly");
    CHECK(g->output_composite != NULL, "output composite created");

    /* Test track management */
    int t0 = wb_edit_add_track(g, "Video 1");
    CHECK(t0 == 0, "first track added");
    int t1 = wb_edit_add_track(g, "Video 2");
    CHECK(t1 == 1, "second track added");
    CHECK(g->track_count == 2, "track count is 2");

    /* Test clip at position (without actual video file) */
    /* We can't test real video without a file, but test the API */
    int ci = wb_edit_clip_at(g, 0, 0.0);
    CHECK(ci == -1, "no clip at t=0 on empty track");

    /* Test clip split on empty track */
    int si = wb_edit_split_clip(g, 0, 0, 1.0);
    CHECK(si == -1, "split on empty track returns -1");

    /* Test move on empty track */
    int mi = wb_edit_move_clip(g, 0, 0, 5.0);
    CHECK(mi == -1, "move on empty track returns -1");

    /* Test transition on empty track */
    int ti = wb_edit_add_transition(g, 0, 0, WB_EDIT_TRANS_CROSSFADE, 1.0);
    CHECK(ti == -1, "transition on empty track returns -1");

    /* Test duration */
    double dur = wb_edit_graph_get_duration(g);
    CHECK(dur == 0.0, "duration is 0 with no clips");

    /* Test evaluation with no clips (should return black frame) */
    wb_frame *f = wb_edit_graph_evaluate(g, 0.0);
    CHECK(f != NULL, "evaluate returns frame even with no clips");
    if (f) {
        CHECK(f->w == 854 && f->h == 480, "frame has correct dimensions");
        wb_frame_free(f);
    }

    /* Test evaluation at various times */
    f = wb_edit_graph_evaluate(g, 5.0);
    CHECK(f != NULL, "evaluate at t=5s");
    if (f) wb_frame_free(f);

    f = wb_edit_graph_evaluate(g, 0.033);  /* frame 1 */
    CHECK(f != NULL, "evaluate at t=0.033s (frame 1)");
    if (f) wb_frame_free(f);

    /* Test track removal */
    wb_edit_remove_track(g, 1);
    CHECK(g->track_count == 1, "track removed");

    /* Test quality setting */
    wb_compositor_set_quality(0.5);
    CHECK(fabs(wb_compositor_get_quality() - 0.5) < 0.001, "quality set to 0.5");
    wb_compositor_set_quality(1.0);

    /* Cleanup */
    wb_edit_graph_destroy(g);
    printf("\n%s\n", pass ? "ALL PASS" : "SOME FAILED");
    return pass ? 0 : 1;
}
