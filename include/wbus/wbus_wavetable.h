#ifndef WBUS_WBUS_WAVETABLE_H
#define WBUS_WBUS_WAVETABLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wavetable synthesizer (Serum/Vital style).
 * Multi-table single-cycle morphing with unison, interpolation, and
 * mipmapped anti-aliasing. Pure C11, zero third-party. */

typedef struct wb_wavetable wb_wavetable;

/* Wavetable preset types for wb_wavetable_generate_wavetable */
typedef enum {
    WT_PRESET_SINE = 0,
    WT_PRESET_SAW,
    WT_PRESET_SQUARE,
    WT_PRESET_TRIANGLE,
    WT_PRESET_PULSE_25,
    WT_PRESET_PULSE_10,
    WT_PRESET_ORGAN,
    WT_PRESET_BRASS,
    WT_PRESET_STRING,
    WT_PRESET_VOCAL,
    WT_PRESET_METAL,
    WT_PRESET_BELL,
    WT_PRESET_COUNT
} wavetable_preset_t;

/* Interpolation modes */
#define WT_INTERP_NEAREST 0
#define WT_INTERP_LINEAR  1
#define WT_INTERP_CUBIC   2

/* Create/destroy */
wb_wavetable *wb_wavetable_create(uint32_t sr);
void           wb_wavetable_destroy(wb_wavetable *wt);

/* Load a wavetable: frames = array of num_frames single-cycle waveforms,
 * each of length table_count samples. Total frames = num_frames * table_count.
 * Returns 0 on success, negative on error. */
int wb_wavetable_load_wavetable(wb_wavetable *wt, const float *frames,
                                int num_frames, int table_count);

/* Generate a preset wavetable with `table_count` morph frames.
 * Returns 0 on success, negative on error. */
int wb_wavetable_generate_wavetable(wb_wavetable *wt, int table_count,
                                    wavetable_preset_t preset);

/* Trigger a note (MIDI note 0-127, velocity 0-127). */
void wb_wavetable_note(wb_wavetable *wt, int note, int vel);

/* Set morph position 0..1 across all tables. */
void wb_wavetable_set_position(wb_wavetable *wt, float pos);

/* Set interpolation mode: 0=nearest, 1=linear, 2=cubic. */
void wb_wavetable_set_interpolation(wb_wavetable *wt, int mode);

/* Render n stereo frames into L and R (interleaved-independent buffers). */
void wb_wavetable_render(wb_wavetable *wt, float *L, float *R, uint32_t n);

/* Set unison voices (1-16) and detune spread (0..1). */
void wb_wavetable_set_unison(wb_wavetable *wt, int voices, float spread);

/* Set filter cutoff (20..20000 Hz) and resonance (0..1). */
void wb_wavetable_set_filter(wb_wavetable *wt, float cutoff, float resonance);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_WBUS_WAVETABLE_H */