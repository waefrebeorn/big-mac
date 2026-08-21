/* test_workspace.c — R043 Fusion-style workspace/tier controller gate.
 * Verifies: create/destroy, default unlock state, activation, the gated
 * (locked) tiers reject activation, and the per-tier predicate helpers.
 * Pure C11, links only wb_workspace.o (self-contained module). */

#include <stdio.h>
#include <string.h>
#include "wbus/wbus_workspace.h"

static int checks = 0, failures = 0;
#define CHECK(c, m) do { checks++; if (!(c)) { failures++; fprintf(stderr, "  [FAIL] %s\n", m); } } while(0)

static int g_changed = 0;
static wb_workspace_tier g_old, g_new;
static void on_change(void *ctx, wb_workspace_tier old_t, wb_workspace_tier new_t) {
    (void)ctx; g_changed++; g_old = old_t; g_new = new_t;
}

int main(void) {
    wb_workspace *ws = wb_workspace_create(on_change, NULL);
    CHECK(ws != NULL, "workspace created");

    CHECK(wb_workspace_active(ws) == WB_WS_AUDIO, "default active is AUDIO");

    /* default unlock: AUDIO/VIDEO/FUSION unlocked, 3D-CGI/AGI locked */
    CHECK(wb_workspace_unlocked(ws, WB_WS_AUDIO),  "AUDIO unlocked by default");
    CHECK(wb_workspace_unlocked(ws, WB_WS_VIDEO),  "VIDEO unlocked by default");
    CHECK(wb_workspace_unlocked(ws, WB_WS_FUSION), "FUSION unlocked by default");
    CHECK(!wb_workspace_unlocked(ws, WB_WS_3DCGI), "3D-CGI locked by default");
    CHECK(!wb_workspace_unlocked(ws, WB_WS_AGI),   "AGI locked by default");

    /* locked tier must reject activation */
    CHECK(wb_workspace_set(ws, WB_WS_3DCGI) != 0, "locked 3D-CGI rejects set");
    CHECK(wb_workspace_active(ws) == WB_WS_AUDIO, "active unchanged after rejected set");

    /* valid activation + change callback fires */
    g_changed = 0;
    CHECK(wb_workspace_set(ws, WB_WS_VIDEO) == 0, "VIDEO activates");
    CHECK(wb_workspace_active(ws) == WB_WS_VIDEO, "active is VIDEO");
    CHECK(g_changed == 1, "on_change fired once");
    CHECK(g_old == WB_WS_AUDIO && g_new == WB_WS_VIDEO, "on_change reports old->new");

    /* per-tier predicates */
    CHECK(!wb_workspace_audio_active(ws), "audio_active false when VIDEO");
    CHECK(wb_workspace_video_active(ws),  "video_active true when VIDEO");

    /* unlock then activate a gated tier */
    wb_workspace_set_unlocked(ws, WB_WS_AGI, 1);
    CHECK(wb_workspace_unlocked(ws, WB_WS_AGI), "AGI unlockable");
    CHECK(wb_workspace_set(ws, WB_WS_AGI) == 0, "AGI activates after unlock");
    CHECK(wb_workspace_agi_active(ws), "agi_active true when AGI");

    /* invalid tier rejected */
    CHECK(wb_workspace_set(ws, (wb_workspace_tier)-1) != 0, "negative tier rejected");
    CHECK(wb_workspace_set(ws, (wb_workspace_tier)99) != 0,  "out-of-range tier rejected");

    /* labels are stable static strings */
    CHECK(strcmp(wb_workspace_label(WB_WS_AUDIO), "AUDIO") == 0, "AUDIO label");
    CHECK(strcmp(wb_workspace_label(WB_WS_FUSION), "FUSION") == 0, "FUSION label");

    wb_workspace_destroy(ws);
    ws = NULL;
    CHECK(wb_workspace_active(ws) == WB_WS_AUDIO, "NULL-safe active returns AUDIO");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
