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

/* R013 mouth: lip rounding (protrusion) 0..1 — purses the terminal lip tube
 * into a horn, which lowers F2/F3 (rounding/protrusion acoustics). This is
 * what separates rounded /u o y/ (and /w/) from spread vowels, and (via the
 * phone table) the /ʃ/ family from /s/. */
void wb_tract_set_lip_rounding(wb_tract_t *t, double amount);

/* R013 mouth: turbulence (frication) source position in section units
 * (0 = glottis .. n-1 = lips). Passing <0 restores the default alveolar
 * position (tip_start). Places labiodental /f v/ noise at the lips and
 * sibilant noise at the teeth, so the front cavity (source..lip) sizes
 * the noise spectral peak. */
void wb_tract_set_noise_pos(wb_tract_t *t, double index); /* <0 = default alveolar */
void wb_tract_set_length_frac(wb_tract_t *t, double frac); /* 0..1 extra section (fractional-delay) */

/* Advance one sample step. Returns lip+nose output. */
double wb_tract_run_step(wb_tract_t *t, double glottal_output, double turbulence_noise, double lambda);

/* End of a processing block (reshape tract toward targets). */
void wb_tract_finish_block(wb_tract_t *t, double block_time);

/* Diagnostics. */
int wb_tract_n(const wb_tract_t *t);

/* Per-section diameter access (for the fitting loop / full control).
 * idx in [0, n). Clamped. */
void wb_tract_set_diameter(wb_tract_t *t, int idx, double d);
double wb_tract_get_diameter(const wb_tract_t *t, int idx);

/* Set every section's target diameter at once (fitting loop uses this). */
void wb_tract_set_all_diameters(wb_tract_t *t, const double *diams, int n);
void wb_tract_get_all_diameters(const wb_tract_t *t, double *diams, int n);

#endif /* WB_TRACT_H */
