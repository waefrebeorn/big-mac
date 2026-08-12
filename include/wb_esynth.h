/* wb_esynth.h — espeak-ng-style additive formant synthesis (R015 fresh start) */
#ifndef WB_ESYNTH_H
#define WB_ESYNTH_H

/* one formant peak: center freq, height, left/right bandwidths (Hz) */
typedef struct {
    double freq;
    double height;
    double left;
    double right;
} wb_esynth_peak_t;

/* per-phone acoustic specification: f0, amplitude, and the formant peaks */
typedef struct {
    double f0;
    double amplitude;
    int npeaks;
    wb_esynth_peak_t peaks[8];
} wb_esynth_phone_t;

typedef struct wb_esynth wb_esynth_t;

wb_esynth_t *wb_esynth_new(int sample_rate);
void wb_esynth_free(wb_esynth_t *s);
/* additive-harmonic render of one phone into out (nsamp samples, added) */
void wb_esynth_phone(wb_esynth_t *s, double *out, int nsamp,
                     const wb_esynth_phone_t *ph);

#endif
