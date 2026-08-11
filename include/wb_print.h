/*
 * wb_print.h — Big Mac voice-print: the measured parameter vector (B11)
 *
 * The "vocal bank" we make programmatically: a voice-print is the set of
 * measurements that fully describes a voice — F0 stats, formants + BW,
 * quality params, and the fitted tract diameters. Save/load as JSON
 * (our own tiny C11 writer — no third party).
 */
#ifndef WB_PRINT_H
#define WB_PRINT_H

#include "wb_measure.h"

typedef struct {
    /* source */
    char name[128];
    /* analysis (from a demo) */
    wb_f0_measure_t f0;
    wb_formant_measure_t formants;
    wb_quality_measure_t quality;
    /* fitted tract (from wb_fit) — 44 diameters */
    double diameters[44];
    int n_diameters;
    double f0_render;   /* glottis frequency used when re-creating */
} wb_voiceprint_t;

/* Save a voice-print as JSON. Returns 0 on success. */
int wb_print_save(const char *path, const wb_voiceprint_t *vp);

/* Load a voice-print from JSON. Returns 0 on success. */
int wb_print_load(const char *path, wb_voiceprint_t *vp);

#endif /* WB_PRINT_H */
