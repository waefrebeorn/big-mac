/* wb_dsp.c — built-in plugin registry. */

#include <stdlib.h>
#include <string.h>
#include "wbus_plugin.h"

#define WB_DSP_MAX 64

static const wb_plugin *registry[WB_DSP_MAX];
static int n = 0;

int wb_dsp_register(const wb_plugin *p) {
    if (!p || !p->vt || n >= WB_DSP_MAX) return 0;
    /* dedupe by id */
    for (int i = 0; i < n; i++)
        if (registry[i]->vt->id && p->vt->id &&
            strcmp(registry[i]->vt->id(p), p->vt->id(p)) == 0)
            return 0;
    registry[n++] = p;
    return 1;
}

const wb_plugin *wb_dsp_find(const char *id) {
    for (int i = 0; i < n; i++) {
        const wb_plugin *p = registry[i];
        if (p->vt->id && p->vt->id(p) && strcmp(p->vt->id(p), id) == 0)
            return p;
    }
    return NULL;
}
