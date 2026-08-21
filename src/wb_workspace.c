/* wb_workspace.c — DaVinci-Resolve-style workspace/tier controller.
 * See wbus_workspace.h. Self-contained: no DAW/UI/render knowledge. Only
 * models active tier + unlock state + change callback. Opaque struct. */

#include "wbus/wbus_workspace.h"
#include <stdlib.h>
#include <string.h>

struct wb_workspace {
    wb_workspace_tier     active;
    int                   unlocked[WB_WS_COUNT];
    wb_workspace_on_change on_change;
    void                 *ctx;
};

static const char *g_labels[WB_WS_COUNT] = {
    "AUDIO", "VIDEO", "FUSION", "3D-CGI", "AGI"
};

wb_workspace *wb_workspace_create(wb_workspace_on_change cb, void *ctx) {
    wb_workspace *ws = calloc(1, sizeof(*ws));
    if (!ws) return NULL;
    /* Default: AUDIO + VIDEO + FUSION unlocked (the combo DAW+NLE core).
     * 3D-CGI and AGI start locked — the user unlocks them when the
     * capability exists (low-poly renderer / AGI API integration). */
    ws->active          = WB_WS_AUDIO;
    ws->unlocked[WB_WS_AUDIO]  = 1;
    ws->unlocked[WB_WS_VIDEO]  = 1;
    ws->unlocked[WB_WS_FUSION] = 1;
    ws->unlocked[WB_WS_3DCGI]  = 0;
    ws->unlocked[WB_WS_AGI]    = 0;
    ws->on_change = cb;
    ws->ctx       = ctx;
    return ws;
}

void wb_workspace_destroy(wb_workspace *ws) {
    if (!ws) return;
    free(ws);
}

int wb_workspace_set(wb_workspace *ws, wb_workspace_tier t) {
    if (!ws || t < 0 || t >= WB_WS_COUNT) return -1;
    if (!ws->unlocked[t]) return -1;            /* gated tier */
    wb_workspace_tier old = ws->active;
    ws->active = t;
    if (t != old && ws->on_change)
        ws->on_change(ws->ctx, old, t);
    return 0;
}

wb_workspace_tier wb_workspace_active(wb_workspace *ws) {
    return ws ? ws->active : WB_WS_AUDIO;
}

void wb_workspace_set_unlocked(wb_workspace *ws, wb_workspace_tier t, int on) {
    if (!ws || t < 0 || t >= WB_WS_COUNT) return;
    ws->unlocked[t] = on ? 1 : 0;
}

int wb_workspace_unlocked(wb_workspace *ws, wb_workspace_tier t) {
    if (!ws || t < 0 || t >= WB_WS_COUNT) return 0;
    return ws->unlocked[t];
}

int wb_workspace_audio_active(wb_workspace *ws) {
    return ws ? (ws->active == WB_WS_AUDIO) : 1;
}
int wb_workspace_video_active(wb_workspace *ws) {
    return ws ? (ws->active == WB_WS_VIDEO) : 0;
}
int wb_workspace_fusion_active(wb_workspace *ws) {
    return ws ? (ws->active == WB_WS_FUSION) : 0;
}
int wb_workspace_cgi_active(wb_workspace *ws) {
    return ws ? (ws->active == WB_WS_3DCGI) : 0;
}
int wb_workspace_agi_active(wb_workspace *ws) {
    return ws ? (ws->active == WB_WS_AGI) : 0;
}

const char *wb_workspace_label(wb_workspace_tier t) {
    if (t < 0 || t >= WB_WS_COUNT) return "?";
    return g_labels[t];
}
