/* wb_unit.c — effect-unit registry + adapters over the built-in DSP.
 * Wraps the existing comp/reverb/delay/sampler units behind the unified
 * wb_unit vtable so the engine's insert chains are data-driven. New units
 * (FM synth, drums, chorus, EQ, CLAP bridge) register here.
 */

#include <stdlib.h>
#include <string.h>

#include "wb_unit.h"
#include "wb_internal.h"

#define WB_UNIT_MAX 32
static const wb_unit *g_units[WB_UNIT_MAX];
static int g_n = 0;

int wb_unit_register(const wb_unit *u) {
    if (!u || !u->vt || !u->vt->id || !u->vt->create || !u->vt->destroy ||
        !u->vt->process || g_n >= WB_UNIT_MAX) return 0;
    for (int i = 0; i < g_n; i++)
        if (g_units[i]->vt->id && strcmp(g_units[i]->vt->id(), u->vt->id()) == 0)
            return 0; /* already registered */
    g_units[g_n++] = u;
    return 1;
}

const wb_unit *wb_unit_find(const char *id) {
    for (int i = 0; i < g_n; i++)
        if (g_units[i]->vt->id && strcmp(g_units[i]->vt->id(), id) == 0)
            return g_units[i];
    return NULL;
}

void *wb_unit_create(const char *id, uint32_t sr) {
    const wb_unit *u = wb_unit_find(id);
    return u ? u->vt->create(sr) : NULL;
}

void wb_unit_destroy(const char *id, void *inst) {
    const wb_unit *u = wb_unit_find(id);
    if (u && inst) u->vt->destroy(inst);
}

void wb_unit_process(const char *id, void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    const wb_unit *u = wb_unit_find(id);
    if (u && inst) u->vt->process(inst, L, R, n);
}

/* ---- generic adapter macro for simple stereo in-place effects ---------- */
#define UNIT(idstr, createf, destroyf, processf)                                    \
static void *u_create_##createf(uint32_t sr){ return createf(sr); }                 \
static void u_destroy_##createf(void *i){ destroyf(i); }                           \
static void u_process_##createf(void *i, wb_sample *L, wb_sample *R, uint32_t n){ processf(i,L,R,n); } \
static const char *u_id_##createf(void){ return idstr; }                           \
static const wb_unit_vtable u_vt_##createf = {                                     \
    u_id_##createf, u_create_##createf, u_destroy_##createf, u_process_##createf, 0,0,0,0 }; \
static const wb_unit u_unit_##createf = { &u_vt_##createf }

/* comp */
UNIT("comp", wb_comp_create, wb_comp_destroy, wb_comp_process);
/* delay */
UNIT("delay", wb_delay_create, wb_delay_destroy, wb_delay_process);
/* reverb */
UNIT("reverb", wb_reverb_create, wb_reverb_destroy, wb_reverb_process);

/* sampler (in-place process + note hook) */
static void *u_sampler_create(uint32_t sr){ return wb_sampler_create(sr); }
static void u_sampler_destroy(void *i){ wb_sampler_destroy(i); }
static void u_sampler_process(void *i, wb_sample *L, wb_sample *R, uint32_t n){ wb_sampler_render(i,L,R,n); }
static void u_sampler_note(void *i, int n, int v){ wb_sampler_note(i,n,v); }
static const char *u_sampler_id(void){ return "sampler"; }
static const wb_unit_vtable u_sampler_vt = {
    u_sampler_id, u_sampler_create, u_sampler_destroy, u_sampler_process,
    u_sampler_note, 0,0,0 };
static const wb_unit u_sampler_unit = { &u_sampler_vt };

/* ---- register everything ------------------------------------------------- */
void wb_unit_ensure_all(void) {
    /* one-time guard via a static int; registration is idempotent anyway */
    static int done = 0;
    if (done) return;
    wb_unit_register(&u_unit_wb_comp_create);
    wb_unit_register(&u_unit_wb_delay_create);
    wb_unit_register(&u_unit_wb_reverb_create);
    wb_unit_register(&u_sampler_unit);
    /* new units self-register through their own ensure hooks */
    wb_unit_ensure_fm();
    wb_unit_ensure_drums();
    wb_unit_ensure_chorus();
    wb_unit_ensure_eq();
    done = 1;
}
