/* wb_osc.c — oscillators: sine/saw/square/triangle/noise.
 * Anti-aliased saw via poly-BLEP-lite (parabolic residual). Sine via
 * phase accumulation. All ours, no libm dependence beyond sqrt/fabs.
 */

#include <math.h>
#include "wbus_dsp.h"

void wb_osc_reset(wb_osc *o) {
    o->phase = 0.0;
    o->phase_inc = 0.0;
    o->last_out = 0.0;
}

/* poly-BLEP residual for band-limited edges */
static float blep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

float wb_osc_process(wb_osc *o, float inc, int waveform, float shape) {
    o->phase += inc;
    if (o->phase >= 2.0 * M_PI) o->phase -= 2.0 * M_PI;
    if (o->phase < 0) o->phase += 2.0 * M_PI;

    float t = (float)(o->phase / (2.0 * M_PI));   /* 0..1 */
    float dt = inc / (float)(2.0 * M_PI);
    float out = 0.0f;

    switch (waveform) {
    case WB_WAVE_SINE:
        out = (float)sin(o->phase);
        break;
    case WB_WAVE_SAW: {
        out = 2.0f * t - 1.0f;                 /* naive saw */
        out -= blep(t, dt);                    /* poly-BLEP correction */
        /* second edge at t=1 (the wrap) handled implicitly */
        break;
    }
    case WB_WAVE_SQUARE: {
        float pw = (shape < 0.01f) ? 0.5f : shape; /* pulse width 0..1 */
        out = (t < pw) ? 1.0f : -1.0f;
        out += blep(t, dt);
        out -= blep(fmodf(t + 1.0f - pw, 1.0f), dt);
        break;
    }
    case WB_WAVE_TRIANGLE: {
        out = 4.0f * fabsf(t - 0.5f) - 1.0f;
        out = out * 0.5f + 0.5f;               /* normalise 0..1 */
        break;
    }
    case WB_WAVE_NOISE:
        out = wb_noise_next();
        break;
    default:
        out = 0.0f;
        break;
    }
    o->last_out = out;
    return out;
}

static unsigned long rng_state = 0x9E3779B9u;

void wb_noise_seed(unsigned long s) { rng_state = s ? s : 0x9E3779B9u; }

float wb_noise_next(void) {
    /* xorshift32 */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (float)((double)(rng_state & 0xFFFFFF) / 8388608.0 - 1.0);
}
