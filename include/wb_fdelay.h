/*
 * wb_fdelay.h — Thiran first-order allpass fractional delay.
 * Delays a signal by a fractional number of samples (0..1) with a flat
 * magnitude response (allpass). Used for continuous vocal-tract length.
 */
#ifndef WB_FDELAY_H
#define WB_FDELAY_H

typedef struct {
    double d;    /* fractional delay in samples, [0,1] */
    double a1;   /* Thiran coefficient */
    double x1;   /* last input */
    double y1;   /* last output */
} wb_fdelay_t;

/* Set the fractional delay d (0..1 samples). */
void wb_fdelay_set(wb_fdelay_t *f, double d);

/* One step: delay x by the configured fractional amount. */
double wb_fdelay_run(wb_fdelay_t *f, double x);

/* Reset internal state to zero. */
void wb_fdelay_reset(wb_fdelay_t *f);

#endif /* WB_FDELAY_H */
