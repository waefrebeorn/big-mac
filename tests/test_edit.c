/* test_edit.c — test the video edit graph (R084) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "wbus/wbus_edit.h"
#include "wbus/wbus_compositor.h"
#include "wbus/wb_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Audio FX functions (from wb_audio_fx.c) */
void wb_audio_fx_eq(float *buf, int n_frames, int channels, int sample_rate,
                    float low_gain, float mid_gain, float high_gain);
void wb_audio_fx_reverb(float *buf, int n_frames, int channels, int sample_rate,
                         float decay, float mix);
void wb_audio_fx_compressor(float *buf, int n_frames, int channels,
                             float threshold, float ratio, float attack,
                             float release);
void wb_audio_fx_delay(float *buf, int n_frames, int channels, int sample_rate,
                        float delay_time, float feedback, float mix);
void wb_audio_fx_distortion(float *buf, int n_frames, int channels,
                             float amount);
void wb_audio_fx_chorus(float *buf, int n_frames, int channels, int sample_rate,
                         float rate, float depth, float mix);

#ifdef __cplusplus
}
#endif

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

    /* ---- auto-cut scenes API contract tests ---- */
    printf("\n-- auto-cut scenes --\n");
    {
        wb_edit_graph *g3 = wb_edit_graph_create(30.0, 854, 480);
        CHECK(g3 != NULL, "edit graph created for auto-cut tests");

        int t = wb_edit_add_track(g3, "V1");
        CHECK(t == 0, "track added for auto-cut tests");

        /* Test: auto-cut on empty track (no clips) returns -1 */
        int cuts0 = wb_edit_auto_cut_scenes(g3, 0, 0, 0.3f);
        CHECK(cuts0 == -1, "auto-cut on empty track returns -1");

        /* Test: auto-cut with invalid track index returns -1 */
        int cuts1 = wb_edit_auto_cut_scenes(g3, -1, 0, 0.3f);
        CHECK(cuts1 == -1, "auto-cut with negative track returns -1");

        int cuts2 = wb_edit_auto_cut_scenes(g3, 99, 0, 0.3f);
        CHECK(cuts2 == -1, "auto-cut with out-of-range track returns -1");

        /* Test: auto-cut with NULL graph returns -1 */
        int cuts3 = wb_edit_auto_cut_scenes(NULL, 0, 0, 0.3f);
        CHECK(cuts3 == -1, "auto-cut with NULL graph returns -1");

        /* Test: auto-cut with invalid clip index (no clips added) returns -1 */
        int cuts4 = wb_edit_auto_cut_scenes(g3, 0, 5, 0.3f);
        CHECK(cuts4 == -1, "auto-cut with invalid clip index returns -1");

        /* Test: auto-cut with threshold clamping (threshold <= 0 gets defaulted) */
        /* We can't test with a real video file, but the API contract for
         * invalid clips is already covered above. The threshold clamping
         * and scene detection path require a valid video file. */

        wb_edit_graph_destroy(g3);
    }

    /* ---- Subtitle burn-in tests ---- */
    printf("\n--- Subtitle burn-in ---\n");
    wb_edit_graph *g3 = wb_edit_graph_create(30.0, 320, 240);
    CHECK(g3 != NULL, "edit graph created for subtitle tests");

    /* Check defaults */
    CHECK(g3->subtitle_text[0] == '\0', "subtitle text empty by default");
    CHECK(g3->subtitle_pos_x == 0.05f, "default pos_x is 0.05");
    CHECK(g3->subtitle_pos_y == 0.85f, "default pos_y is 0.85");
    CHECK(g3->subtitle_size == 2.0f, "default size is 2.0");
    CHECK(g3->subtitle_color == 0xFFFFFFFF, "default color is white");

    /* Test set_subtitle */
    wb_edit_set_subtitle(g3, "Hello World");
    CHECK(strcmp(g3->subtitle_text, "Hello World") == 0, "subtitle text set");
    wb_edit_set_subtitle(g3, "");
    CHECK(g3->subtitle_text[0] == '\0', "subtitle cleared with empty string");
    wb_edit_set_subtitle(g3, NULL);
    CHECK(g3->subtitle_text[0] == '\0', "subtitle cleared with NULL");
    wb_edit_set_subtitle(g3, "Test Caption");
    CHECK(strcmp(g3->subtitle_text, "Test Caption") == 0, "subtitle text re-set");

    /* Test set_subtitle_position */
    wb_edit_set_subtitle_position(g3, 0.5f, 0.5f);
    CHECK(g3->subtitle_pos_x == 0.5f, "pos_x set to 0.5");
    CHECK(g3->subtitle_pos_y == 0.5f, "pos_y set to 0.5");
    /* Test clamping */
    wb_edit_set_subtitle_position(g3, -1.0f, 2.0f);
    CHECK(g3->subtitle_pos_x == 0.0f, "pos_x clamped to 0");
    CHECK(g3->subtitle_pos_y == 1.0f, "pos_y clamped to 1");

    /* Test set_subtitle_size */
    wb_edit_set_subtitle_size(g3, 3.0f);
    CHECK(g3->subtitle_size == 3.0f, "size set to 3.0");
    wb_edit_set_subtitle_size(g3, 0);
    CHECK(g3->subtitle_size == 1.0f, "size clamped to 1.0 for 0");
    wb_edit_set_subtitle_size(g3, -5);
    CHECK(g3->subtitle_size == 1.0f, "size clamped to 1.0 for negative");

    /* Test set_subtitle_color */
    wb_edit_set_subtitle_color(g3, 0xFF0000);
    CHECK(g3->subtitle_color == 0xFF0000FF, "color set with alpha forced to 0xFF");
    wb_edit_set_subtitle_color(g3, 0x00FF00AA);
    CHECK((g3->subtitle_color & 0xFF) == 0xFF, "alpha always forced to 0xFF");

    /* Test that subtitle doesn't crash evaluate (render path) */
    wb_edit_set_subtitle(g3, "Test");
    wb_frame *fs = wb_edit_graph_evaluate(g3, 0.0);
    CHECK(fs != NULL, "evaluate with subtitle set returns frame");
    if (fs) wb_frame_free(fs);

    /* Test with empty subtitle (should not crash) */
    wb_edit_set_subtitle(g3, "");
    fs = wb_edit_graph_evaluate(g3, 0.0);
    CHECK(fs != NULL, "evaluate with empty subtitle returns frame");
    if (fs) wb_frame_free(fs);

    /* Test with NULL graph (should not crash) */
    wb_edit_set_subtitle(NULL, "test");
    wb_edit_set_subtitle_position(NULL, 0.5f, 0.5f);
    wb_edit_set_subtitle_size(NULL, 2.0f);
    wb_edit_set_subtitle_color(NULL, 0xFFFFFFFF);
    printf("ok: NULL graph calls don't crash\n");

    /* Test long text (truncation) */
    char long_text[512];
    memset(long_text, 'A', 511);
    long_text[511] = '\0';
    wb_edit_set_subtitle(g3, long_text);
    CHECK(strlen(g3->subtitle_text) < 256, "long text truncated to buffer size");

    wb_edit_graph_destroy(g3);

    /* ---- Audio clip management tests ---- */
    printf("\n--- Audio clip management ---\n");
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        CHECK(g != NULL, "edit graph created for audio tests");

        int t = wb_edit_add_track(g, "Audio Track");
        CHECK(t == 0, "track added for audio tests");

        /* Test: add audio clip */
        int ai = wb_edit_add_audio_clip(g, 0, "/tmp/test_audio.wav", 0.0, 5.0, 0.0);
        CHECK(ai == 0, "audio clip added to track 0");
        CHECK(g->tracks[0].audio_clip_count == 1, "audio clip count is 1");

        /* Test: audio clip fields set correctly */
        wb_edit_audio_clip *ac = &g->tracks[0].audio_clips[0];
        CHECK(strcmp(ac->source_path, "/tmp/test_audio.wav") == 0, "audio clip source path correct");
        CHECK(ac->start_in_source == 0.0, "audio clip start_in_source correct");
        CHECK(ac->duration == 5.0, "audio clip duration correct");
        CHECK(ac->timeline_pos == 0.0, "audio clip timeline_pos correct");
        CHECK(ac->volume == 1.0f, "audio clip default volume is 1.0");
        CHECK(ac->speed == 1.0f, "audio clip default speed is 1.0");

        /* Test: timeline duration updated by audio clip */
        CHECK(g->duration == 5.0, "timeline duration updated by audio clip");

        /* Test: add second audio clip */
        int ai2 = wb_edit_add_audio_clip(g, 0, "/tmp/test_audio2.wav", 2.0, 3.0, 5.0);
        CHECK(ai2 == 1, "second audio clip added");
        CHECK(g->tracks[0].audio_clip_count == 2, "audio clip count is 2");
        CHECK(g->duration == 8.0, "timeline duration updated for second audio clip");

        /* Test: set audio volume */
        int rc = wb_edit_set_audio_volume(g, 0, 0, 0.5f);
        CHECK(rc == 0, "set audio volume succeeds");
        CHECK(g->tracks[0].audio_clips[0].volume == 0.5f, "audio volume set to 0.5");

        /* Test: set audio volume clamping (negative -> 0) */
        rc = wb_edit_set_audio_volume(g, 0, 0, -1.0f);
        CHECK(rc == 0, "set negative volume succeeds (clamped)");
        CHECK(g->tracks[0].audio_clips[0].volume == 0.0f, "negative volume clamped to 0");

        /* Test: set volume on invalid clip */
        rc = wb_edit_set_audio_volume(g, 0, 99, 0.5f);
        CHECK(rc == -1, "set volume on invalid clip returns -1");

        /* Test: set volume on invalid track */
        rc = wb_edit_set_audio_volume(g, 99, 0, 0.5f);
        CHECK(rc == -1, "set volume on invalid track returns -1");

        /* Test: add audio clip with invalid track */
        int ai3 = wb_edit_add_audio_clip(g, 99, "/tmp/x.wav", 0.0, 1.0, 0.0);
        CHECK(ai3 == -1, "add audio clip to invalid track returns -1");

        /* Test: add audio clip with zero duration */
        int ai4 = wb_edit_add_audio_clip(g, 0, "/tmp/x.wav", 0.0, 0.0, 0.0);
        CHECK(ai4 == -1, "add audio clip with zero duration returns -1");

        /* Test: add audio clip with NULL source */
        int ai5 = wb_edit_add_audio_clip(g, 0, NULL, 0.0, 1.0, 0.0);
        CHECK(ai5 == -1, "add audio clip with NULL source returns -1");

        /* Test: add audio clip with NULL graph */
        int ai6 = wb_edit_add_audio_clip(NULL, 0, "/tmp/x.wav", 0.0, 1.0, 0.0);
        CHECK(ai6 == -1, "add audio clip with NULL graph returns -1");

        /* Test: set volume with NULL graph */
        rc = wb_edit_set_audio_volume(NULL, 0, 0, 0.5f);
        CHECK(rc == -1, "set volume with NULL graph returns -1");

        wb_edit_graph_destroy(g);
    }

    /* ---- Save/Load ---- */
    printf("\n--- Save/Load ---\n");
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        int t0 = wb_edit_add_track(g, "V1");
        CHECK(t0 == 0, "save-load: track added");

        const char *tmp_path = "/tmp/test_edit_save.bedit";
        int rc = wb_edit_graph_save(g, tmp_path);
        CHECK(rc == 0, "save: graph saved");

        wb_edit_graph *loaded = wb_edit_graph_load(tmp_path);
        CHECK(loaded != NULL, "load: graph loaded");
        if (loaded) {
            CHECK(loaded->fps == 30.0, "load: fps preserved");
            CHECK(loaded->width == 854, "load: width preserved");
            CHECK(loaded->height == 480, "load: height preserved");
            CHECK(loaded->track_count == 1, "load: track count preserved");
            wb_edit_graph_destroy(loaded);
        }

        wb_edit_graph_destroy(g);
    }

    /* ---- Keyframe animation ---- */
    printf("\n--- Keyframe animation ---\n");
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        int t0 = wb_edit_add_track(g, "V1");
        CHECK(t0 == 0, "keyframe: track added");

        /* Add a color source and an FX node */
        wb_node *color = wb_node_source_color(0.5f, 0.5f, 0.5f, 1.0f, 64, 36);
        wb_node *fx = wb_node_effect_deep_fry();
        wb_node_connect(fx, color, 0);
        wb_edit_clip *cl = &g->tracks[0].clips[0];  /* No clip, but test keyframe API */

        /* Test: set keyframe on non-existent clip should fail */
        int rc = wb_edit_set_keyframe(g, 0, 0, 0, "intensity", 1.0, 2.0f);
        CHECK(rc == -1, "keyframe on clip without FX returns -1");

        /* Test: set keyframe on invalid track should fail */
        rc = wb_edit_set_keyframe(g, 99, 0, 0, "intensity", 1.0, 2.0f);
        CHECK(rc == -1, "keyframe on invalid track returns -1");

        /* Test: get keyframe with no keyframes returns 0 */
        float v = wb_edit_get_keyframed_value(g, 0, 0, 0, "intensity", 1.0);
        CHECK(v == 0.0f, "get keyframe with no keyframes returns 0");

        /* Test: NULL graph doesn't crash */
        rc = wb_edit_set_keyframe(NULL, 0, 0, 0, "intensity", 1.0, 2.0f);
        CHECK(rc == -1, "set keyframe with NULL graph returns -1");
        v = wb_edit_get_keyframed_value(NULL, 0, 0, 0, "intensity", 1.0);
        CHECK(v == 0.0f, "get keyframe with NULL graph returns 0");

        wb_node_destroy(fx);
        wb_node_destroy(color);
        wb_edit_graph_destroy(g);
    }

    /* ---- Undo/Redo ---- */
    printf("\n--- Undo/Redo ---\n");
    {
        wb_edit_undo_init();
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        CHECK(g != NULL, "undo: graph created");

        /* Initial state: 0 tracks */
        wb_edit_undo_checkpoint();
        wb_edit_undo_set_current(g);
        CHECK(g->track_count == 0, "undo: initial state has 0 tracks");

        /* Add track */
        wb_edit_undo_checkpoint();
        wb_edit_add_track(g, "V1");
        wb_edit_undo_set_current(g);
        CHECK(g->track_count == 1, "undo: after add track, count is 1");

        /* Add another track */
        wb_edit_undo_checkpoint();
        wb_edit_add_track(g, "V2");
        wb_edit_undo_set_current(g);
        CHECK(g->track_count == 2, "undo: after second track, count is 2");

        /* Can undo? */
        CHECK(wb_edit_undo_can_undo(), "undo: can undo after changes");

        /* Undo once */
        wb_edit_graph *prev = wb_edit_undo_undo(g);
        CHECK(prev != NULL, "undo: undo returns previous state");
        if (prev) {
            CHECK(prev->track_count == 1, "undo: previous state has 1 track");
            wb_edit_graph_destroy(g);
            g = prev;
        }

        /* Undo again */
        prev = wb_edit_undo_undo(g);
        CHECK(prev != NULL, "undo: second undo returns state");
        if (prev) {
            CHECK(prev->track_count == 0, "undo: initial state has 0 tracks");
            wb_edit_graph_destroy(g);
            g = prev;
        }

        /* Can't undo past beginning */
        CHECK(!wb_edit_undo_can_undo(), "undo: can't undo past beginning");

        /* Can redo? */
        CHECK(wb_edit_undo_can_redo(), "undo: can redo");

        /* Redo */
        wb_edit_graph *next = wb_edit_undo_redo(g);
        CHECK(next != NULL, "undo: redo returns next state");
        if (next) {
            CHECK(next->track_count == 1, "undo: redo restores 1 track");
            wb_edit_graph_destroy(g);
            g = next;
        }

        wb_edit_graph_destroy(g);
        wb_edit_undo_shutdown();
    }

    /* ---- Multi-camera ---- */
    printf("\n--- Multi-camera ---\n");
    {
        wb_multicam_clear();
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        int t0 = wb_edit_add_track(g, "Cam A");
        int t1 = wb_edit_add_track(g, "Cam B");
        CHECK(t0 == 0 && t1 == 1, "multicam: 2 tracks added");

        /* Test: create group with no clips should fail */
        int tracks[] = {0, 1};
        int clips[] = {0, 0};
        int gi = wb_multicam_create_group(g, "test", tracks, clips, 2, 0.0);
        CHECK(gi == -1, "multicam: group with no clips returns -1");

        /* Test: set active angle on invalid group */
        int rc = wb_multicam_set_active_angle(0, 0);
        CHECK(rc == -1, "multicam: set angle on invalid group returns -1");

        /* Test: get path from invalid group */
        const char *path = wb_multicam_get_active_path(0);
        CHECK(path == NULL, "multicam: get path from invalid group returns NULL");

        /* Test: count is 0 when no groups */
        CHECK(wb_multicam_count() == 0, "multicam: count is 0 initially");

        wb_multicam_clear();
        wb_edit_graph_destroy(g);
    }

    /* ---- Audio FX ---- */
    printf("\n--- Audio FX ---\n");
    {
        /* Test EQ on a sine wave */
        int sr = 48000;
        int n = 4800;
        int ch = 2;
        float *buf = (float *)malloc(n * ch * sizeof(float));
        for (int i = 0; i < n; i++) {
            float s = sinf(2.0f * 3.14159f * 1000.0f * i / sr) * 0.5f;
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }

        /* Apply EQ: boost low, cut high */
        wb_audio_fx_eq(buf, n, ch, sr, 2.0f, 1.0f, 0.5f);
        CHECK(fabsf(buf[100]) < 1.0f, "EQ: output not clipped");

        /* Apply reverb */
        wb_audio_fx_reverb(buf, n, ch, sr, 0.5f, 0.3f);
        CHECK(fabsf(buf[100]) < 1.0f, "Reverb: output not clipped");

        /* Apply compressor */
        wb_audio_fx_compressor(buf, n, ch, 0.5f, 4.0f, 0.005f, 0.12f);
        CHECK(fabsf(buf[100]) < 1.0f, "Compressor: output not clipped");

        /* Apply delay */
        wb_audio_fx_delay(buf, n, ch, sr, 0.1f, 0.3f, 0.3f);
        CHECK(fabsf(buf[100]) < 1.0f, "Delay: output not clipped");

        /* Apply distortion */
        wb_audio_fx_distortion(buf, n, ch, 0.5f);
        CHECK(fabsf(buf[100]) <= 1.0f, "Distortion: soft clip works");

        /* Apply chorus */
        wb_audio_fx_chorus(buf, n, ch, sr, 1.5f, 0.005f, 0.3f);
        CHECK(fabsf(buf[100]) < 1.0f, "Chorus: output not clipped");

        /* Test API: set FX on a clip */
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        int t0 = wb_edit_add_track(g, "Audio");
        /* Can't add real audio clip without file, but test FX slot API */
        wb_audio_fx fx;
        memset(&fx, 0, sizeof(fx));
        fx.type = WB_AUDIO_FX_REVERB;
        fx.enabled = 1;
        fx.reverb.room_size = 0.5f;
        fx.reverb.wet = 0.3f;

        /* This will fail because there are no audio clips, which is expected */
        int rc = wb_edit_set_audio_fx(g, 0, 0, 0, &fx);
        CHECK(rc == -1, "audio FX on non-existent clip returns -1");

        /* Test clear FX */
        rc = wb_edit_clear_audio_fx(g, 0, 0, 0);
        CHECK(rc == -1, "clear FX on non-existent clip returns -1");

        wb_edit_graph_destroy(g);
        free(buf);
    }

    /* ---- Noise Gate ---- */
    printf("\n--- Noise Gate ---\n");
    {
        float *noise = (float *)malloc(4800 * sizeof(float));
        for (int i = 0; i < 4800; i++)
            noise[i] = 0.01f * ((float)rand() / RAND_MAX - 0.5f);
        wb_noise_gate *ng = wb_noise_gate_create(48000.0f);
        CHECK(ng != NULL, "noise gate created");
        int rc = wb_noise_gate_learn(ng, noise, 4800, 1, 512);
        CHECK(rc == 0, "noise profile learned");
        /* Add a signal that should pass through */
        for (int i = 2048; i < 4096; i++)
            noise[i] += 0.5f * sinf(2.0f * M_PI * 440.0f * i / 48000.0f);
        rc = wb_noise_gate_process(ng, noise, 4800, 1);
        CHECK(rc == 0, "noise gate applied");
        wb_noise_gate_destroy(ng);
        free(noise);
    }

    /* ---- Shape Nodes ---- */
    printf("\n--- Shape Nodes ---\n");
    {
        wb_node *rect = wb_node_source_shape_rect(100, 100);
        CHECK(rect != NULL, "rect shape created");
        wb_frame *f = wb_node_pull(rect, 0, 0, 0, 100, 100);
        CHECK(f != NULL, "rect frame pulled");
        if (f) {
            /* Center pixel should be filled */
            CHECK(f->px[50 * 100 + 50].a > 0.5f, "rect center filled");
            /* Corner pixel should be transparent (rounded) */
            CHECK(f->px[0].a < 0.5f, "rect corner empty");
            wb_frame_free(f);
        }
        rect->free(rect);

        wb_node *ellipse = wb_node_source_shape_ellipse(100, 100);
        CHECK(ellipse != NULL, "ellipse shape created");
        f = wb_node_pull(ellipse, 0, 0, 0, 100, 100);
        CHECK(f != NULL, "ellipse frame pulled");
        if (f) {
            CHECK(f->px[50 * 100 + 50].a > 0.5f, "ellipse center filled");
            CHECK(f->px[0 * 100 + 0].a < 0.5f, "ellipse corner empty");
            wb_frame_free(f);
        }
        ellipse->free(ellipse);

        wb_node *poly = wb_node_source_shape_polygon(100, 100, 6);
        CHECK(poly != NULL, "polygon shape created");
        f = wb_node_pull(poly, 0, 0, 0, 100, 100);
        CHECK(f != NULL, "polygon frame pulled");
        if (f) {
            CHECK(f->px[50 * 100 + 50].a > 0.5f, "polygon center filled");
            wb_frame_free(f);
        }
        poly->free(poly);

        wb_node *star = wb_node_source_shape_star(100, 100, 5, 0.4f, 1.0f);
        CHECK(star != NULL, "star shape created");
        f = wb_node_pull(star, 0, 0, 0, 100, 100);
        CHECK(f != NULL, "star frame pulled");
        if (f) {
            CHECK(f->px[50 * 100 + 50].a > 0.5f, "star center filled");
            wb_frame_free(f);
        }
        star->free(star);

        /* Test fill/stroke colors */
        wb_node *colored = wb_node_source_shape_rect(50, 50);
        wb_node_shape_set_fill(colored, 1.0f, 0.0f, 0.0f, 1.0f);
        wb_node_shape_set_stroke(colored, 0.0f, 1.0f, 0.0f, 1.0f, 2.0f);
        f = wb_node_pull(colored, 0, 0, 0, 50, 50);
        CHECK(f != NULL, "colored rect pulled");
        if (f) {
            CHECK(f->px[25 * 50 + 25].r > 0.9f, "fill color red");
            CHECK(f->px[25 * 50 + 25].g < 0.1f, "fill color not green");
            wb_frame_free(f);
        }
        colored->free(colored);
    }

    /* ---- BWF (Broadcast Wave Format) ---- */
    printf("\n--- BWF ---\n");
    {
        /* Generate a simple sine wave */
        int sr = 48000;
        int n_frames = 4800;  /* 0.1 seconds */
        int ch = 2;
        float *data = (float *)malloc(n_frames * ch * sizeof(float));
        for (int i = 0; i < n_frames; i++) {
            float s = sinf(2.0f * 3.14159f * 440.0f * i / sr) * 0.5f;
            data[i * 2] = s;
            data[i * 2 + 1] = s;
        }

        const char *bwf_path = "/tmp/test_bwf.wav";
        int rc = wb_wav_write_bwf(bwf_path, data, n_frames, ch, sr,
                                   "Big Mac Test", "BigMac", "REF001",
                                   time(NULL));
        CHECK(rc == 0, "BWF: file written");

        /* Verify it can be read back as a regular WAV */
        float *read_data = NULL;
        uint32_t read_frames = 0;
        int read_ch = 0, read_sr = 0;
        rc = wb_wav_read_pcm16(bwf_path, &read_data, &read_frames, &read_ch, &read_sr);
        CHECK(rc == 0, "BWF: readable as WAV");
        CHECK(read_frames == (uint32_t)n_frames, "BWF: frame count preserved");
        CHECK(read_ch == ch, "BWF: channel count preserved");
        CHECK(read_sr == sr, "BWF: sample rate preserved");
        if (read_data) {
            /* Check first sample is close to 0 (sine starts at 0) */
            CHECK(fabsf(read_data[0]) < 0.01f, "BWF: first sample near zero");
            free(read_data);
        }

        free(data);
    }

    printf("\n%s\n", pass ? "ALL PASS" : "SOME FAILED");
    return pass ? 0 : 1;
}
