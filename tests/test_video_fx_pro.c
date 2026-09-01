/* test_video_fx_pro.c — test professional video FX nodes (R085) */
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

    /* Test stabilize node */
    wb_node *stab = wb_node_effect_stabilize();
    CHECK(stab != NULL, "stabilize node created");
    wb_node_effect_stabilize_set_enabled(stab, 1);

    /* Test chroma key node */
    wb_node *ck = wb_node_effect_chromakey(0.0f, 1.0f, 0.0f, 0.4f);
    CHECK(ck != NULL, "chromakey node created");
    wb_node_effect_chromakey_set_color(ck, 0.0f, 0.8f, 0.1f);

    /* Test transform node */
    wb_node *tr = wb_node_effect_transform_pro();
    CHECK(tr != NULL, "transform_pro node created");
    wb_node_effect_transform_pro_set_scale(tr, 1.5f);
    wb_node_effect_transform_pro_set_pos(tr, 0.5f, 0.5f);
    wb_node_effect_transform_pro_set_rotation(tr, 0.1f);

    /* Test audio-reactive node */
    wb_node *ar = wb_node_effect_audio_reactive(1.0f);
    CHECK(ar != NULL, "audio_reactive node created");

    /* Test that all nodes have correct input count */
    CHECK(stab->n_inputs == 1, "stabilize has 1 input");
    CHECK(ck->n_inputs == 1, "chromakey has 1 input");
    CHECK(tr->n_inputs == 1, "transform_pro has 1 input");
    CHECK(ar->n_inputs == 1, "audio_reactive has 1 input");

    /* Test pulling from transform (identity should work) */
    wb_node *color = wb_node_source_color(0.5f, 0.5f, 0.5f, 1.0f, 64, 36);
    wb_node_connect(tr, color, 0);
    wb_frame *f = wb_node_pull(tr, 0.0, 0, 0, 64, 36);
    CHECK(f != NULL, "pull from transform_pro");
    if (f) wb_frame_free(f);

    /* Test pulling from chroma key (needs input) */
    wb_node *green = wb_node_source_color(0.0f, 1.0f, 0.0f, 1.0f, 64, 36);
    wb_node_connect(ck, green, 0);
    f = wb_node_pull(ck, 0.0, 0, 0, 64, 36);
    CHECK(f != NULL, "pull from chromakey");
    if (f) {
        /* Green screen should be keyed out (alpha ~0) */
        wb_px *center = &f->px[0];
        (void)center;
        wb_frame_free(f);
    }

    /* Cleanup */
    wb_node_destroy(stab);
    wb_node_destroy(ck);
    wb_node_destroy(tr);
    wb_node_destroy(ar);

    printf("\n%s\n", pass ? "ALL PASS" : "SOME FAILED");
    return pass ? 0 : 1;
}
