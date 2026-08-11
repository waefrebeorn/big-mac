/*
 * wb_tract.h — Big Mac vocal tract (Kelly-Lochbaum waveguide)
 *
 * The resonant filter of the voice: 44 tube sections whose diameters
 * shape the formants. A digital waveguide (ladder filter) with a nasal
 * side branch. Ported to strict C11 from the Pink Trombone reference
 * (Neil Thapen, MIT) — SLERMed, byte by byte, no third party.
 */
#ifndef WB_TRACT_H
#define WB_TRACT_H

#include <stddef.h>

typedef struct wb_tract wb_tract_t;

/* Create a tract with n sections (44 = male, 36 = female, 28 = child). */
wb_tract_t *wb_tract_new(int n);

void wb_tract_free(wb_tract_t *t);

/* Articulatory controls (the teeth/lips/tongue/velum). */
void wb_tract_set_rest_diameter(wb_tract_t *t, double tongue_index, double tongue_diameter);
void wb_tract_set_lips(wb_tract_t *t, double aperture);   /* 0 closed .. 1 open */
void wb_tract_set_teeth(wb_tract_t *t, double gap);       /* dental constriction */
void wb_tract_set_velum(wb_tract_t *t, double openness);  /* nasal coupling */
void wb_tract_set_constriction(wb_tract_t *t, double index, double diameter, double width);

/* Advance one sample step. Returns lip+nose output. */
double wb_tract_run_step(wb_tract_t *t, double glottal_output, double turbulence_noise, double lambda);

/* End of a processing block (reshape tract toward targets). */
void wb_tract_finish_block(wb_tract_t *t, double block_time);

/* Diagnostics. */
int wb_tract_n(const wb_tract_t *t);

#endif /* WB_TRACT_H */
