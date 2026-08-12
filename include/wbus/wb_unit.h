#ifndef WUBUS_WB_UNIT_H
#define WUBUS_WB_UNIT_H

/* Unified effect-unit interface (internal).
 * Every insert effect exposes the same vtable; the engine walks an insert
 * chain by calling process() on each slot's unit — data-driven, no hardcoded
 * slot->type switch. New effects register here and drop into any slot.
 */

#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_unit wb_unit;

typedef struct wb_unit_vtable {
    const char *(*id)(void);
    void *(*create)(uint32_t sr);
    void  (*destroy)(void *inst);
    /* stereo in-place process: reads+writes L/R, n frames, sample-accurate */
    void  (*process)(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
    /* optional: react to a MIDI note (drums/arpeggiators). May be NULL. */
    void  (*note)(void *inst, int note, int vel);
    /* optional: expose a named parameter (0..1 normalized). May be NULL. */
    int   (*has_param)(const void *inst, const char *name);
    void  (*set_param)(void *inst, const char *name, float v01);
    float (*get_param)(const void *inst, const char *name);
} wb_unit_vtable;

struct wb_unit {
    const wb_unit_vtable *vt;
};

/* registry */
int  wb_unit_register(const wb_unit *u);
const wb_unit *wb_unit_find(const char *id);

/* built-in units register themselves on first use */
void wb_unit_ensure_all(void);
void wb_unit_ensure_fm(void);
void wb_unit_ensure_drums(void);
void wb_unit_ensure_chorus(void);
void wb_unit_ensure_eq(void);

/* convenience: create/process/destroy via unit id */
void *wb_unit_create(const char *id, uint32_t sr);
void  wb_unit_destroy(const char *id, void *inst);
void  wb_unit_process(const char *id, void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* ---- instrument dispatch: units that carry note() + render ----------------
 * Some units are "instruments" (poly synth, FM, drums) that get note events
 * from the scheduler and render audio into a track buffer. This wraps a unit
 * instance into a uniform (note, render) pair the engine can call without
 * caring about the concrete type.
 */
typedef void (*wb_inst_note_fn)(void *inst, int note, int vel);
typedef void (*wb_inst_render_fn)(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

typedef struct {
    void *inst;
    wb_inst_note_fn  note;
    wb_inst_render_fn render;
    void (*destroy)(void *inst);
} wb_instrument;

/* Build a uniform instrument from a unit id. Returns NULL if the unit has
 * no note() support. The builtin "synth" maps to the legacy poly synth. */
wb_instrument *wb_instrument_make(const char *id, uint32_t sr);
void          wb_instrument_destroy(wb_instrument *inst);
/* proxy callable from the scheduler's 3-arg callback shape */
void          wb_unit_note_proxy(void *voice, int note, int vel);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WB_UNIT_H */
