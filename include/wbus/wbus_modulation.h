#ifndef WUBUS_WBUS_MODULATION_H
#define WUBUS_WBUS_MODULATION_H

/* Big Mac DAW — Modulation Matrix (unified modulation system).
 *
 * Inspired by Bitwig's "anything can modulate anything" design: a global
 * modulation matrix routes modulation SOURCES (LFO, Envelope, Step) to any
 * parameter DESTINATION (any track insert-slot parameter, built-in FX or VST3).
 *
 * Each block the engine evaluates every active source, scales its output by the
 * route amount, and writes the resulting normalized value to the destination
 * parameter via the same path automation/UI use (wb_engine_set_insert_param).
 *
 * This is sample-block-accurate: sources advance by `frames` each call.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WB_MOD_NONE = 0,
    WB_MOD_LFO,    /* sine LFO, rate in Hz */
    WB_MOD_ENV,    /* looping ADSR envelope, retriggered per cycle */
    WB_MOD_STEP    /* 8-step bipolar sequencer */
} wb_mod_src_type;

/* A modulation source instance. */
typedef struct wb_mod_src {
    wb_mod_src_type type;
    float rate;            /* LFO: Hz; ENV: full-cycle Hz; STEP: steps/sec */
    float depth;            /* output scale, 0..1 (bipolar sources already -1..1) */
    float phase;            /* internal phase accumulator, 0..1 */
    /* ENV shape */
    float a, d, s, r;      /* attack/decay/sustain/release in 0..1 of cycle */
    /* STEP pattern (8 entries, each -1..1) */
    float step[8];
    int   step_idx;
    int   enabled;
} wb_mod_src;

/* A modulation route: source -> destination parameter. */
typedef struct wb_mod_route {
    int   src;             /* index into the engine's source list */
    int   track;           /* destination track */
    int   slot;            /* destination insert slot */
    int   param;           /* destination param index (0-based) */
    float amount;          /* modulation depth, -1..1 (sign = invert) */
    float base;            /* base value when mod is at zero (0..1) */
    int   enabled;
} wb_mod_route;

/* Create a modulation source. Returns a pointer owned by the engine; the
 * index is its stable id for routing. Caller sets type/params after. */
wb_mod_src *wb_mod_src_create(wb_mod_src_type type);
void        wb_mod_src_destroy(wb_mod_src *s);

/* Evaluate one source over `frames` samples, advancing its phase. Returns the
 * current instantaneous output in -1..1 (already depth-scaled). */
float wb_mod_src_eval(wb_mod_src *s, uint32_t frames, float sample_rate);

/* Matrix: a set of routes + sources evaluated per block. */
typedef struct wb_mod_matrix wb_mod_matrix;

wb_mod_matrix *wb_mod_matrix_create(void);
void           wb_mod_matrix_destroy(wb_mod_matrix *m);

int  wb_mod_matrix_add_src(wb_mod_matrix *m, wb_mod_src *s);   /* returns src id */
int  wb_mod_matrix_add_route(wb_mod_matrix *m, const wb_mod_route *r); /* returns route id */
void wb_mod_matrix_clear(wb_mod_matrix *m);

/* Evaluate the whole matrix for one block, writing destinations via the
 * provided setter callback (so the matrix stays decoupled from the engine).
 * setter(ctx, track, slot, param, value01). */
typedef void (*wb_mod_setter)(void *ctx, int track, int slot, int param, float value01);
void wb_mod_matrix_eval(wb_mod_matrix *m, uint32_t frames, float sample_rate,
                        wb_mod_setter setter, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_MODULATION_H */
