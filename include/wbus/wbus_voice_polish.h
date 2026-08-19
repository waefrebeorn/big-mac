/* wbus_voice_polish.h — one-click "make voice sound right" preset chain.
 *
 * A single-call processor that chains, in order:
 *   1. noise gate      (kill room tone / breaths between phrases)
 *   2. de-esser        (tame sibilance around 5-8 kHz)
 *   3. compressor      (even out levels, catch peaks)
 *   4. 2-band tilt EQ  (gentle presence lift + low-cut)
 *   5. limiter         (safety ceiling)
 *   6. loudness norm    (ITU-R BS.1770-ish: scale to -16 LUFS target)
 *
 * Pure C11, zero third-party. Reuses wb_biquad for EQ. Target: the
 * Hindenburg/Descript "one button fix voice" feature (R015 Tier 1).
 */

#ifndef WUBUS_WBUS_VOICE_POLISH_H
#define WUBUS_WBUS_VOICE_POLISH_H

#include <stdint.h>
#include <stddef.h>
#include "wbus/wbus_param_track.h"   /* G7: param-track-driven graph */

typedef struct wb_voice_polish wb_voice_polish;

/* Create a voice-polish instance for the given sample rate + channels. */
wb_voice_polish *wb_voice_polish_create(float sample_rate, int channels);
void             wb_voice_polish_free(wb_voice_polish *vp);

/* Reset internal state (call between independent clips). */
void wb_voice_polish_reset(wb_voice_polish *vp);

/* ---- G7: param-track-driven graph --------------------------------------
 * Each stage parameter can be bound to a keyframed wb_param_track so the
 * polish chain animates over time (e.g. compressor ratio ramps up on a
 * loud section). A bound track overrides the static value. Unbound params
 * use the static value set via wb_voice_polish_set(). */
int  wb_voice_polish_bind(wb_voice_polish *vp, const char *param,
                          wb_param_track *tr);
float wb_voice_polish_param_at(wb_voice_polish *vp, const char *param, double t);

/* Set a static stage parameter (dB / ratio / LUFS). Names:
 *   "gate_thresh" (dBFS), "deess_thresh" (dB), "comp_thresh" (dBFS),
 *   "comp_ratio", "lim_ceiling" (dBFS), "eq_presence" (dB), "target_lufs". */
void wb_voice_polish_set(wb_voice_polish *vp, const char *param, float value);

/* Process `frames` of interleaved float audio in place.
 * channels must match the create() value. Returns 0 on success. */
int wb_voice_polish_process(wb_voice_polish *vp, float *buf, uint32_t frames);

/* Convenience: process a whole mono/stereo buffer end-to-end.
 * Measures loudness, applies gate→...→limiter, then scales to target LUFS.
 * buf is interleaved frames*channels floats. Returns 0 on success. */
int wb_voice_polish_apply(float *buf, uint32_t frames, int channels,
                           float sample_rate, float target_lufs);

/* Measure integrated loudness (LUFS, BS.1770 K-weighted) of a buffer.
 * Exposed for tests/UI. Returns LUFS (negative dB). */
float wb_loudness_measure(const float *buf, uint32_t frames, int channels,
                           float sample_rate);

#endif /* WUBUS_WBUS_VOICE_POLISH_H */
