/*
 * wb_retrieve.h — formant → articulation RETRIEVAL TABLE (shallow, no NN)
 *
 * The "shallow retrieval" of the backwards-RVC voice changer: a precomputed
 * table mapping (F1, F2) target formants to the 3 articulatory knobs
 * (tongue index, tongue diameter, lip aperture) that produce them. Built
 * offline by running the 3-parameter fitting loop on a grid; looked up at
 * runtime with bilinear interpolation — fast enough for real-time on one core.
 */
#ifndef WB_RETRIEVE_H
#define WB_RETRIEVE_H

#define WB_RETRIEVE_MAX_N 32

typedef struct {
    int n1, n2;              /* grid dims */
    double f1_lo, f1_hi;     /* F1 range */
    double f2_lo, f2_hi;     /* F2 range */
    double ti[WB_RETRIEVE_MAX_N][WB_RETRIEVE_MAX_N];  /* tongue index */
    double td[WB_RETRIEVE_MAX_N][WB_RETRIEVE_MAX_N];  /* tongue diameter */
    double lips[WB_RETRIEVE_MAX_N][WB_RETRIEVE_MAX_N];/* lip aperture */
} wb_retrieve_t;

/* Run the 3-parameter fit for ONE target formant pair. Renders + measures
 * internally (a few seconds each). Returns 0 on success. */
int wb_retrieve_fit_pair(double f1, double f2,
                         double *ti, double *td, double *lips);

/* Build the full table on an n1 x n2 grid of (F1, F2). */
int wb_retrieve_build(wb_retrieve_t *t, int n1, int n2,
                      double f1_lo, double f1_hi, double f2_lo, double f2_hi);

/* Save table to a file (text, so it's inspectable). */
int wb_retrieve_save(const char *path, const wb_retrieve_t *t);

/* Load table from file. */
int wb_retrieve_load(const char *path, wb_retrieve_t *t);

/* Bilinear lookup: (F1, F2) -> knobs. Returns 0 if inside the grid. */
int wb_retrieve_lookup(const wb_retrieve_t *t, double f1, double f2,
                       double *ti, double *td, double *lips);

#endif /* WB_RETRIEVE_H */
