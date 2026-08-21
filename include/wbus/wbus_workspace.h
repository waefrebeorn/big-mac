/* wbus_workspace.h — DaVinci-Resolve-style workspace/tier switcher.
 *
 * Big Mac is a COMBO DAW + NLE + (future) 3D-CGI + AGI control surface.
 * The user asked for a Fusion-style bottom ribbon that cleanly flips the
 * app between workspaces (AUDIO / VIDEO / FUSION / 3D-CGI / AGI) so audio
 * and video work never get tangled, while sharing ONE session + engine.
 *
 * Design rules (WuBu doctrine): opaque struct, minimal includes, C11 only,
 * no god header. This module is SELF-CONTAINED — it knows nothing about the
 * DAW's tabs, mixer, or renderer. It only models which workspace is active,
 * which tiers are unlocked, and a clean callback whenever the active tier
 * changes. The UI (wb_daw.c) draws the ribbon and calls these functions;
 * the engine/render path queries wb_workspace_audio_active() etc.
 */

#ifndef WBUS_WORKSPACE_H
#define WBUS_WORKSPACE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Workspace tiers. Ordered roughly by pipeline depth.
 *   AUDIO   — recording / mixing / instruments / MIDI (the DAW core)
 *   VIDEO   — timeline NLE, preview, captions, export (the film core)
 *   FUSION  — node compositor / OFX FX graph (shared with VIDEO+3D)
 *   3D-CGI  — low-poly CGI scene (future; AGI-driven generation stub)
 *   AGI     — AI orchestration / control tier (API-level integration)
 * A tier being "active" simply means the ribbon has selected it; the app
 * can show one workspace at a time while keeping all session data live. */
typedef enum {
    WB_WS_AUDIO = 0,
    WB_WS_VIDEO,
    WB_WS_FUSION,
    WB_WS_3DCGI,
    WB_WS_AGI,
    WB_WS_COUNT
} wb_workspace_tier;

/* Opaque workspace controller. Created/destroyed by the host; never
 * inspected directly. All state is private to wb_workspace.c. */
typedef struct wb_workspace wb_workspace;

/* Callback fired whenever the active tier changes. ctx is the user pointer
 * passed at creation. old_tier may equal new_tier on the first activation.
 * The host uses this to lazily load/rebuild the appropriate view state. */
typedef void (*wb_workspace_on_change)(void *ctx, wb_workspace_tier old_tier,
                                       wb_workspace_tier new_tier);

/* Create a workspace controller. on_change may be NULL. Returns NULL on OOM. */
wb_workspace *wb_workspace_create(wb_workspace_on_change cb, void *ctx);

/* Destroy, freeing all internal state. Safe to call with NULL. */
void wb_workspace_destroy(wb_workspace *ws);

/* Set the active tier (ribbon click). Returns 0 on success, -1 on invalid
 * tier. Fires on_change if the tier actually changed. */
int wb_workspace_set(wb_workspace *ws, wb_workspace_tier t);

/* Get the active tier. */
wb_workspace_tier wb_workspace_active(wb_workspace *ws);

/* Mark a tier as unlocked/locked. Tiers are unlocked by default except
 * 3D-CGI and AGI, which are gated behind capabilities the user enables.
 * Trying to activate a locked tier returns -1 from wb_workspace_set. */
void wb_workspace_set_unlocked(wb_workspace *ws, wb_workspace_tier t, int on);
int  wb_workspace_unlocked(wb_workspace *ws, wb_workspace_tier t);

/* Per-tier predicate helpers for the engine/render path. These let the
 * audio stages skip video-only work and vice-versa cheaply. */
int wb_workspace_audio_active(wb_workspace *ws);
int wb_workspace_video_active(wb_workspace *ws);
int wb_workspace_fusion_active(wb_workspace *ws);
int wb_workspace_cgi_active(wb_workspace *ws);
int wb_workspace_agi_active(wb_workspace *ws);

/* Human-readable label for a tier (e.g. "AUDIO", "VIDEO", "FUSION",
 * "3D-CGI", "AGI"). Returns a static string; do not free. */
const char *wb_workspace_label(wb_workspace_tier t);

#ifdef __cplusplus
}
#endif
#endif /* WBUS_WORKSPACE_H */
