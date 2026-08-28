/* tools/compat_wb_drum_test.h — minimal type stubs to compile
 * wb_drums.c in isolation for timing tests. Provides only what
 * wb_drums.c's #include chain needs: wb_sample, wbus.h types,
 * wb_unit.h types, and wb_midi_note_to_freq. */
#ifndef TOOLS_COMPAT_WB_DRUM_TEST_H
#define TOOLS_COMPAT_WB_DRUM_TEST_H

#include <stdint.h>

/* ---- wbus.h subset --------------------------------------------------- */
typedef float wb_sample;
typedef uint32_t wb_audio_block;  /* not actually used by drums.c */

/* ---- wbus_midifx.h subset ------------------------------------------- */
/* (empty — drums.c doesn't use midifx types directly) */

/* ---- wb_unit.h subset ----------------------------------------------- */
typedef struct wb_unit wb_unit;
typedef struct wb_unit_vtable {
    const char *(*id)(void);
    void *(*create)(uint32_t sr);
    void  (*destroy)(void *inst);
    void  (*process)(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
    void  (*note)(void *inst, int note, int vel);
    int   (*has_param)(const void *inst, const char *name);
    void  (*set_param)(void *inst, const char *name, float v01);
    float (*get_param)(const void *inst, const char *name);
} wb_unit_vtable;

typedef const wb_unit_vtable *wb_unit_vtable_ptr;
typedef struct wb_unit { wb_unit_vtable_ptr vt; } wb_unit;

/* ---- wb_midi_note_to_freq ------------------------------------------- */
double wb_midi_note_to_freq(int note);

#endif /* TOOLS_COMPAT_WB_DRUM_TEST_H */
