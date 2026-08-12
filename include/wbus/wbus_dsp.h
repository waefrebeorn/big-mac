#ifndef WUBUS_WBUS_DSP_H
#define WUBUS_WBUS_DSP_H

/* Big Mac DAW — DSP unit helpers and registrations.
 * wb_track_runtime lives in wb_core.c; this header exposes the helper
 * primitives (osc, env, filters) used by the instrument/effect units.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- oscillator ------------------------------------------------------- */
typedef struct wb_osc {
    double phase;
    double phase_inc;
    double last_out;   /* for anti-aliased saw */
    float  detune;
} wb_osc;

void wb_osc_reset(wb_osc *o);
/* returns one sample of the requested waveform at the current phase,
 * advancing phase by `inc` (radians/sample). */
float wb_osc_process(wb_osc *o, float inc, int waveform, float shape);

#define WB_WAVE_SINE 0
#define WB_WAVE_SAW  1
#define WB_WAVE_SQUARE 2
#define WB_WAVE_TRIANGLE 3
#define WB_WAVE_NOISE 4

/* ---- ADSR envelope ---------------------------------------------------- */
typedef struct wb_env {
    float a, d, s, r;      /* times in seconds (r also) */
    float sr;
    double level;
    int   stage;           /* 0 idle, 1 attack, 2 decay, 3 sustain, 4 release */
    int   note_on;
    double cur;            /* stage clock */
} wb_env;

void wb_env_init(wb_env *e, float sr);
void wb_env_note_on(wb_env *e, float a, float d, float s, float r);
void wb_env_note_off(wb_env *e);
float wb_env_process(wb_env *e);

/* ---- biquad filter ---------------------------------------------------- */
typedef struct wb_biquad {
    float sr;
    float b0,b1,b2,a1,a2;
    float x1,x2,y1,y2;
} wb_biquad;

void wb_biquad_init(wb_biquad *f, float sr);
/* type: 0 lowpass, 1 highpass, 2 bandpass, 3 notch */
void wb_biquad_set(wb_biquad *f, int type, float freq, float q, float gain_db);
float wb_biquad_process(wb_biquad *f, float x);

/* ---- simple noise (white) --------------------------------------------- */
float wb_noise_next(void);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_DSP_H */
