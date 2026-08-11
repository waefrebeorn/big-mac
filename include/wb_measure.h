/*
 * wb_measure.h — Big Mac voice-print measurement library (strict C11)
 *
 * Everything needed to ABSORB a voice: F0, formants, jitter/shimmer/HNR,
 * spectral tilt, CPP, H1-H2. Used by wb_analyze and by the fitting loop
 * (wb_fit) — the same measure feeds both absorb and verify.
 */
#ifndef WB_MEASURE_H
#define WB_MEASURE_H

#include <stddef.h>

typedef struct {
    double f0;                 /* YIN F0, Hz (0 = unvoiced) */
    double f0_mean, f0_min, f0_max, f0_sd;
    double vibrato_rate;       /* Hz */
    double vibrato_depth;      /* cents */
    double voiced_fraction;    /* 0..1 */
} wb_f0_measure_t;

typedef struct {
    double F[4];               /* formants Hz, 0 = not found */
    double BW[4];              /* bandwidths Hz */
    int n;                     /* how many found */
} wb_formant_measure_t;

typedef struct {
    double jitter_pct;         /* local jitter % */
    double shimmer_pct;        /* local shimmer % */
    double hnr_db;             /* harmonic-to-noise, dB */
    double cpp;                /* cepstral peak prominence */
    double h1h2_db;            /* H1-H2 (open quotient correlate) */
    double tilt_db_per_oct;    /* spectral tilt */
} wb_quality_measure_t;

/* Analyze one buffer. All functions take mono samples in [-1,1]. */
wb_f0_measure_t wb_measure_f0(const double *x, size_t n, int sr);
wb_formant_measure_t wb_measure_formants(const double *x, size_t n, int sr);
wb_quality_measure_t wb_measure_quality(const double *x, size_t n, int sr);
double wb_measure_cpp(const double *x, size_t n, int sr);

#endif /* WB_MEASURE_H */
