/* wb_filter.c — biquad filters (lowpass/highpass/bandpass/notch) via
 * RBJ audio EQ cookbook. Used for synth filter and EQ effects.
 */

#include <math.h>
#include "wbus_dsp.h"

void wb_biquad_init(wb_biquad *f, float sr) {
    f->sr = sr;
    f->b0=f->b1=f->b2=f->a1=f->a2=0.0f;
    f->x1=f->x2=f->y1=f->y2=0.0f;
}

void wb_biquad_set(wb_biquad *f, int type, float freq, float q, float gain_db) {
    float sr = f->sr;
    float A = (float)pow(10.0, gain_db / 40.0);
    float w0 = (float)(2.0 * M_PI * freq / sr);
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * q);

    float norm;
    switch (type) {
    case 0: /* lowpass */
        norm = 1.0f / (1.0f + alpha);
        f->b0 = (1.0f - cw) * 0.5f * norm;
        f->b1 = (1.0f - cw) * norm;
        f->b2 = (1.0f - cw) * 0.5f * norm;
        f->a1 = (-2.0f * cw) * norm;
        f->a2 = (1.0f - alpha) * norm;
        break;
    case 1: /* highpass */
        norm = 1.0f / (1.0f + alpha);
        f->b0 = (1.0f + cw) * 0.5f * norm;
        f->b1 = -(1.0f + cw) * norm;
        f->b2 = (1.0f + cw) * 0.5f * norm;
        f->a1 = (-2.0f * cw) * norm;
        f->a2 = (1.0f - alpha) * norm;
        break;
    case 2: /* bandpass (constant 0 dB peak gain) */
        norm = 1.0f / (1.0f + alpha);
        f->b0 = alpha * norm;
        f->b1 = 0.0f;
        f->b2 = -alpha * norm;
        f->a1 = (-2.0f * cw) * norm;
        f->a2 = (1.0f - alpha) * norm;
        break;
    case 3: /* notch */
        norm = 1.0f / (1.0f + alpha);
        f->b0 = 1.0f * norm;
        f->b1 = (-2.0f * cw) * norm;
        f->b2 = 1.0f * norm;
        f->a1 = (-2.0f * cw) * norm;
        f->a2 = (1.0f - alpha) * norm;
        break;
    default:
        break;
    }
}

float wb_biquad_process(wb_biquad *f, float x) {
    float y = f->b0*x + f->b1*f->x1 + f->b2*f->x2 - f->a1*f->y1 - f->a2*f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}
