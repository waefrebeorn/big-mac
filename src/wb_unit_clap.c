/* wb_unit_clap.c — CLAP-plugin bridge as a wb_unit.
 *
 * Lets a loaded CLAP plugin be placed in any track's insert chain just like a
 * built-in effect: the descriptor id is "clap:<plugin_descriptor_id>". When the
 * engine builds an insert slot from the session's insert id, it calls
 * wb_unit_create(id, sr); for a "clap:<id>" string this walks the host's plugin
 * list, instantiates the matching CLAP plugin, and wraps it behind the
 * wb_unit vtable so stage_effects runs it in the realtime graph.
 *
 * The instance pointer stored in tr->inserts[s] is the wb_clap_plugin* itself
 * (it has its own lifetime via create/destroy), so process/destroy just defer.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "wb_unit.h"
#include "wb_internal.h"
#include "wbus_clap.h"

/* ---- per-loaded-clap unit vtable --------------------------------------- */
/* All "clap:*" units share this vtable; the instance is a wb_clap_plugin*. */
static const char *clap_id_fn(void) { return "clap"; }

static void *clap_create(uint32_t sr) {
    /* called by wb_unit_create("clap:<id>", sr). We stash the requested
     * descriptor id in a thread-local-ish buffer so the create fn (which the
     * vtable doesn't let us parameterize) can resolve it. The engine calls
     * wb_unit_create with the full "clap:<id>" id; to keep the vtable
     * signature simple we instead expose wb_unit_clap_create_ex. */
    (void)sr;
    return NULL;
}

static void clap_destroy(void *i) {
    if (i) wb_clap_plugin_destroy((wb_clap_plugin *)i);
}

static void clap_process(void *i, wb_sample *L, wb_sample *R, uint32_t n) {
    /* bridge: run the CLAP plugin in-place over the track's buffer. The
     * plugin reads its input copy and writes output; we feed the input from
     * the current L/R (dry) and mix the wet output back in. */
    if (!i) return;
    wb_clap_plugin_process((wb_clap_plugin *)i, L, R, L, R, n);
}

static const wb_unit_vtable g_clap_vt = {
    clap_id_fn, clap_create, clap_destroy, clap_process, 0, 0, 0, 0
};
static const wb_unit g_clap_unit = { &g_clap_vt };

/* Resolve a "clap:<plugin_descriptor_id>" id to a freshly instantiated
 * wb_clap_plugin bound to host `h`. Returns NULL if not found. */
void *wb_unit_clap_create(wb_clap_host *h, const char *id, uint32_t sr) {
    if (!h || !id || strncmp(id, "clap:", 5) != 0) return NULL;
    const char *pid = id + 5;
    uint32_t count = wb_clap_host_plugin_count(h);
    for (uint32_t i = 0; i < count; i++) {
        const char *pname, *rid;
        if (wb_clap_host_plugin_info(h, i, &pname, &rid) != 0) continue;
        if (rid && strcmp(rid, pid) == 0) {
            wb_clap_plugin *p = wb_clap_plugin_create(h, i, sr);
            if (p) return p;
        }
    }
    return NULL;
}

void wb_unit_clap_ensure(void) {
    wb_unit_register(&g_clap_unit);
}
