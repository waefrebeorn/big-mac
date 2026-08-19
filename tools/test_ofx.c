/* test_ofx.c — headless verification of the minimal OpenFX host (R017 G4).
 * Loads the builtin Brightness plugin, wraps it as a compositor node, and
 * verifies the full Load->Describe->CreateInstance->Render round-trip:
 *   - brightness 1.0  => output == input (identity contract)
 *   - brightness 2.0  => output == 2x input (param via ParameterSuite)
 *   - keyframed "Brightness" track bound to the node (G11 bus) overrides. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "wbus/wbus.h"
#include "wbus/wbus_compositor.h"
#include "wbus/wbus_ofx.h"
#include "wbus/wbus_param_track.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== OpenFX host test (G4) ===\n\n");

    wb_ofx_host *host = wb_ofx_host_create();
    CHECK(host != NULL, "OFX host created (5 suites installed)");

    wb_ofx_plugin *plug = wb_ofx_load_plugin(host, 0);
    CHECK(plug != NULL, "loaded builtin Brightness plugin (Load+Describe)");
    if (!plug) return 1;

    /* source: solid grey 0.5 frame */
    wb_node *src = wb_node_source_color(0.5f, 0.5f, 0.5f, 1.0f, 16, 16);
    CHECK(src != NULL, "source node created");

    wb_node *fx = wb_ofx_node_create(plug, "brightness");
    CHECK(fx != NULL, "OFX plugin wrapped as compositor node");
    fx->inputs[0] = src;
    fx->n_inputs = 1;

    /* pull at brightness 1.0 (default): should be identity */
    wb_frame *o1 = wb_node_pull(fx, 0.0, 0, 0, 16, 16);
    CHECK(o1 != NULL, "Render produced a frame (CreateInstance+Render)");
    if (o1) {
        float v = o1->px[3*o1->w + 3].r;
        CHECK(fabsf(v - 0.5f) < 1e-3f, "brightness 1.0 => identity (0.5->0.5)");
        CHECK(fabsf(o1->px[0].r - 0.5f) < 1e-3f, "identity holds at pixel 0");
        wb_frame_free(o1);
    }

    /* raise brightness to 2.0 via the plugin's param (G11 static bus) */
    wb_ofx_plugin_set_param(plug, "Brightness", 2.0);
    wb_frame *o2 = wb_node_pull(fx, 0.0, 0, 0, 16, 16);
    CHECK(o2 != NULL, "Render at brightness 2.0 ok");
    if (o2) {
        float v = o2->px[3*o2->w + 3].r;
        CHECK(fabsf(v - 1.0f) < 1e-3f, "brightness 2.0 => 0.5*2 clamped to 1.0");
        wb_frame_free(o2);
    }

    /* G11: bind a keyframed "Brightness" track (0.0 -> 1.0 over 1s) */
    wb_param_track *tr = wb_param_track_create();
    wb_param_track_set(tr, 0.0, 0.0f, WB_KF_LINEAR);
    wb_param_track_set(tr, 1.0, 1.0f, WB_KF_LINEAR);
    int bnd = wb_ofx_node_bind_param(fx, "Brightness", tr);
    CHECK(bnd == 0, "bound keyframe track to OFX 'Brightness' param (shared bus)");
    /* at t=0 track says 0.0 -> output black; override default 2.0 */
    wb_ofx_plugin_set_param(plug, "Brightness", 2.0);   /* would give 1.0 if track absent */
    wb_frame *o3 = wb_node_pull(fx, 0.0, 0, 0, 16, 16);
    CHECK(o3 != NULL, "Render with bound keyframe track ok");
    if (o3) {
        float v = o3->px[3*o3->w + 3].r;
        CHECK(fabsf(v - 0.0f) < 1e-3f, "keyframe 0.0 => output black (track wins)");
        wb_frame_free(o3);
    }
    wb_frame *o4 = wb_node_pull(fx, 1.0, 0, 0, 16, 16);
    if (o4) {
        float v = o4->px[3*o4->w + 3].r;
        CHECK(fabsf(v - 0.5f) < 1e-3f, "keyframe 1.0 => identity 0.5 at t=1");
        wb_frame_free(o4);
    }

    wb_param_track_free(tr);
    wb_node_destroy(fx);          /* frees src (composite? no—fx is EFFECT) */
    wb_node_destroy(src);
    wb_ofx_plugin_free(plug);
    wb_ofx_host_free(host);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
