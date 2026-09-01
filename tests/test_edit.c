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

    /* ---- nested sequence tests ---- */
    printf("\n-- nested sequence --\n");

    /* Test sequence creation */
    wb_edit_sequence *seq = wb_edit_sequence_create(30.0, 640, 360);
    CHECK(seq != NULL, "sequence created");
    CHECK(seq->graph != NULL, "sequence has inner graph");
    CHECK(seq->source_node != NULL, "sequence has source node");
    CHECK(seq->graph->fps == 30.0, "sequence inner graph fps correct");
    CHECK(seq->graph->width == 640, "sequence inner graph width correct");
    CHECK(seq->graph->height == 360, "sequence inner graph height correct");

    /* Test sequence_graph accessor */
    wb_edit_graph *inner = wb_edit_sequence_graph(seq);
    CHECK(inner == seq->graph, "sequence_graph returns inner graph");

    /* Test sequence_node accessor */
    wb_node *seq_node = wb_edit_sequence_node(seq);
    CHECK(seq_node != NULL, "sequence_node returns non-null");
    CHECK(seq_node == seq->source_node, "sequence_node is the source node");

    /* Test pulling from the sequence source node (empty graph = black frame) */
    wb_frame *sf = wb_node_pull(seq_node, 0.0, 0, 0, 640, 360);
    CHECK(sf != NULL, "sequence source pull returns frame");
    if (sf) {
        CHECK(sf->w == 640 && sf->h == 360, "sequence frame has correct dimensions");
        wb_frame_free(sf);
    }

    /* Test adding a track and clip to the inner graph */
    int st = wb_edit_add_track(inner, "Seq Track");
    CHECK(st == 0, "track added to inner sequence graph");

    /* Add a color source node to the inner graph via a clip (no real file) */
    /* We can't use wb_edit_add_clip without a video file, so test the graph eval */
    double inner_dur = wb_edit_graph_get_duration(inner);
    CHECK(inner_dur == 0.0, "inner graph duration is 0 with no clips");

    /* Evaluate the inner graph directly */
    wb_frame *inner_frame = wb_edit_graph_evaluate(inner, 0.0);
    CHECK(inner_frame != NULL, "inner graph evaluate returns frame");
    if (inner_frame) wb_frame_free(inner_frame);

    /* Test sequence destroy */
    wb_edit_sequence_destroy(seq);
    printf("ok: sequence destroyed\n");

    /* Test create with default parameters */
    wb_edit_sequence *seq2 = wb_edit_sequence_create(0, 0, 0);
    CHECK(seq2 != NULL, "sequence create with defaults");
    if (seq2) {
        CHECK(seq2->graph->fps == 30.0, "default fps is 30");
        CHECK(seq2->graph->width == 854, "default width is 854");
        CHECK(seq2->graph->height == 480, "default height is 480");
        wb_edit_sequence_destroy(seq2);
    }

    /* ---- Color management pipeline tests ---- */
    printf("\n--- Color management ---\n");
    wb_edit_graph *g2 = wb_edit_graph_create(30.0, 854, 480);
    CHECK(g2 != NULL, "edit graph created for CM tests");

    /* Check defaults */
    CHECK(g2->color_management_enabled == 0, "CM disabled by default");
    CHECK(g2->input_cs == WB_CS_SRGB_TO_LINEAR, "default input_cs is SRGB_TO_LINEAR");
    CHECK(g2->output_cs == WB_CS_LINEAR_TO_SRGB, "default output_cs is LINEAR_TO_SRGB");
    CHECK(g2->tonemap == WB_TM_NONE, "default tonemap is NONE");
    CHECK(g2->cs_node != NULL, "colorspace node created");
    CHECK(g2->tm_node != NULL, "tonemap node created");
    CHECK(g2->post_output != NULL, "post_output endpoint created");

    /* Test enable/disable */
    wb_edit_set_color_management(g2, 1);
    CHECK(g2->color_management_enabled == 1, "CM enabled via API");
    wb_edit_set_color_management(g2, 0);
    CHECK(g2->color_management_enabled == 0, "CM disabled via API");

    /* Test set_input_colorspace */
    wb_edit_set_input_colorspace(g2, WB_CS_PQ_TO_LINEAR);
    CHECK(g2->input_cs == WB_CS_PQ_TO_LINEAR, "input_cs set to PQ_TO_LINEAR");
    CHECK(g2->cs_node != NULL, "cs_node recreated after mode change");
    wb_edit_set_input_colorspace(g2, WB_CS_SRGB_TO_LINEAR);
    CHECK(g2->input_cs == WB_CS_SRGB_TO_LINEAR, "input_cs restored to SRGB_TO_LINEAR");

    /* Test set_output_colorspace */
    wb_edit_set_output_colorspace(g2, WB_CS_LINEAR_TO_PQ);
    CHECK(g2->output_cs == WB_CS_LINEAR_TO_PQ, "output_cs set to LINEAR_TO_PQ");
    wb_edit_set_output_colorspace(g2, WB_CS_LINEAR_TO_SRGB);
    CHECK(g2->output_cs == WB_CS_LINEAR_TO_SRGB, "output_cs restored to LINEAR_TO_SRGB");

    /* Test set_tonemap */
    wb_edit_set_tonemap(g2, WB_TM_REINHARD);
    CHECK(g2->tonemap == WB_TM_REINHARD, "tonemap set to REINHARD");
    CHECK(g2->post_output == g2->tm_node, "post_output points to tm_node after recreate");
    wb_edit_set_tonemap(g2, WB_TM_ACES);
    CHECK(g2->tonemap == WB_TM_ACES, "tonemap set to ACES");
    wb_edit_set_tonemap(g2, WB_TM_NONE);
    CHECK(g2->tonemap == WB_TM_NONE, "tonemap restored to NONE");

    /* Test evaluate with CM enabled (no clips = black frame, should not crash) */
    wb_edit_set_color_management(g2, 1);
    wb_frame *fcm = wb_edit_graph_evaluate(g2, 0.0);
    CHECK(fcm != NULL, "evaluate with CM enabled returns frame");
    if (fcm) wb_frame_free(fcm);

    /* Test evaluate with CM disabled */
    wb_edit_set_color_management(g2, 0);
    fcm = wb_edit_graph_evaluate(g2, 0.0);
    CHECK(fcm != NULL, "evaluate with CM disabled returns frame");
    if (fcm) wb_frame_free(fcm);

    /* Test that CM doesn't corrupt the graph after multiple toggles */
    wb_edit_set_color_management(g2, 1);
    wb_edit_set_input_colorspace(g2, WB_CS_HLG_TO_LINEAR);
    wb_edit_set_tonemap(g2, WB_TM_REINHARD);
    fcm = wb_edit_graph_evaluate(g2, 0.5);
    CHECK(fcm != NULL, "evaluate after CM config changes");
    if (fcm) wb_frame_free(fcm);

    wb_edit_set_color_management(g2, 0);
    fcm = wb_edit_graph_evaluate(g2, 0.5);
    CHECK(fcm != NULL, "evaluate after CM toggle off");
    if (fcm) wb_frame_free(fcm);

    wb_edit_graph_destroy(g2);

    printf("\n%s\n", pass ? "ALL PASS" : "SOME FAILED");
    return pass ? 0 : 1;
}
