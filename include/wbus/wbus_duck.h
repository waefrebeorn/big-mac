/* wbus_duck.h — auto-ducking: music dips under voice (R063).
 *
 * The AV-queuing feature every editor expects: when the voiceover speaks,
 * the music bed drops; when it pauses, music comes back. Implemented as an
 * ENVELOPE GENERATOR (not a live sidechain): we analyze the voice track's
 * loudness envelope, compute a smooth 0..1 duck curve, and write it into
 * a wb_automation_lane on the music track. Deterministic, inspectable in
 * the UI (R047 lane overlay), editable after generation, and applied by
 * the existing automation render path — no new DSP in the hot loop.
 *
 * Pipeline:
 *   voice PCM -> RMS per block (block = ~20ms) -> gate threshold
 *   -> hysteresis smoothing (attack/release ramps) -> gain curve
 *   -> wb_automation_lane points
 *
 * C11, opaque, self-contained.
 */
#ifndef WUBUS_WBUS_DUCK_H
#define WUBUS_WBUS_DUCK_H

#include <stdint.h>
#include <stddef.h>
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float duck_amount;    /* 0..1, how far music drops (1.0 = to silence) */
    float threshold;      /* voice RMS gate, 0..1 (default ~0.05) */
    float attack_ms;      /* fade-in of the dip (default 120ms) */
    float release_ms;     /* fade-out of the dip (default 400ms) */
} wb_duck_params;

/* Defaults: duck to 25% remaining, gentle timing (Premiere-ish). */
wb_duck_params wb_duck_default_params(void);

/* Analyze voice audio and write ducking automation onto `music_lane`.
 * - voice: mono PCM of the voiceover track
 * - n: frame count
 * - sr: sample rate
 * - music_lane: lane that stage_automation will apply to the music track
 * Returns number of automation points written, or -1 on error.
 * The lane is NOT cleared first; call wb_automation_lane_reset or pass a
 * fresh lane. */
int wb_duck_generate(const wb_sample *voice, uint32_t n, uint32_t sr,
                     const wb_duck_params *p, wb_automation_lane *lane);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_DUCK_H */
