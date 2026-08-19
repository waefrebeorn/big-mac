/* wbus_ofx.h — minimal OpenFX (OFX) image-effect host (R017 G4).
 *
 * Loads OFX plugins (conforming to OfxPlugin) and wraps them as wb_nodes
 * in the pull-compositor. Reuses the compositor's RoI/tile/identity contract
 * and the shared param bus (G11): an OFX parameter is surfaced as a
 * wb_param_track so it animates like any other node param.
 *
 * Scope: a *minimal* but spec-faithful host implementing the suites OFX
 * effects actually need — Property, Parameter, ImageEffect, Memory, TimeLine.
 * It is enough to run real Fusion/Resolve-style effects that use
 * GetRegionOfDefinition / clipGetImage / paramGetValueAtTime / IsIdentity.
 */

#ifndef WUBUS_WBUS_OFX_H
#define WUBUS_WBUS_OFX_H

#include "wbus/wbus_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_ofx_host wb_ofx_host;
typedef struct wb_ofx_plugin wb_ofx_plugin;   /* opaque host-side plugin instance */

/* Create/destroy the OFX host (installs the 5 suites). */
wb_ofx_host *wb_ofx_host_create(void);
void         wb_ofx_host_free(wb_ofx_host *h);

/* Load a plugin by calling OfxGetPlugin(index). If binary_path is non-NULL
 * it is recorded for provenance; the plugin symbol is resolved in-process
 * (the builtin test plugin registers itself). Returns an opaque instance or
 * NULL on failure. */
wb_ofx_plugin *wb_ofx_load_plugin(wb_ofx_host *h, int plugin_index);
void           wb_ofx_plugin_free(wb_ofx_plugin *p);

/* Wrap a loaded OFX plugin as a compositor node (its inputs are the same
 * wb_node* array the compositor uses). The node's pull() runs the plugin's
 * kOfxActionRender at the requested roi/time. Returns a node the caller
 * owns (add as a child of a composite/effect graph). */
wb_node *wb_ofx_node_create(wb_ofx_plugin *p, const char *id);

/* Surface an OFX parameter as a keyframed wb_param_track on the node so it
 * rides the shared bus (G11). Param name must match the plugin's declared
 * parameter (e.g. "Brightness"). Returns 0 on success. */
int wb_ofx_node_bind_param(wb_node *node, const char *ofx_param,
                            wb_param_track *tr);

/* Set a static OFX parameter value on the plugin (e.g. "Brightness").
 * Used by the host/UI; a bound keyframe track overrides it per-time. */
void wb_ofx_plugin_set_param(wb_ofx_plugin *p, const char *name, double value);

#ifdef __cplusplus
}
#endif
#endif /* WUBUS_WBUS_OFX_H */
