/*
 * wb_fdelay.c — Thiran first-order allpass fractional delay (KL physical
 * accuracy). Lets a delay line be a fractional number of samples, so the
 * vocal-tract length can vary continuously (not just in integer sections).
 *
 * The first-order Thiran allpass realizes a fractional delay d (in samples):
 *     H(z) = (a1 + z^-1) / (1 + a1*z^-1),  a1 = (1-d)/(1+d)
 * which delays the signal by d samples with a flat magnitude response
 * (allpass). d in [0,1); delays >= 1 use integer delay + this fractional part.
 * Pure C11, self-contained.
 */
#include "wb_fdelay.h"

void wb_fdelay_set(wb_fdelay_t *f, double d) {
    if (d < 0) d = 0;
    if (d > 1) d = 1;
    f->d = d;
    f->a1 = (1.0 - d) / (1.0 + d);
    f->x1 = 0.0;
    f->y1 = 0.0;
}

double wb_fdelay_run(wb_fdelay_t *f, double x) {
    /* y[n] = a1*(x[n] - y[n-1]) + x[n-1]   (first-order Thiran allpass) */
    double y = f->a1 * (x - f->y1) + f->x1;
    f->x1 = x;
    f->y1 = y;
    return y;
}

void wb_fdelay_reset(wb_fdelay_t *f) {
    f->x1 = 0.0;
    f->y1 = 0.0;
}
