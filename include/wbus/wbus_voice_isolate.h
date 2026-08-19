#ifndef WBUS_WBUS_VOICE_ISOLATE_H
#define WBUS_WBUS_VOICE_ISOLATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* R018-D: spectral voice isolation / noise suppression.
 * Pure-C11 STFT + per-bin noise-floor tracking + Wiener-style soft gain
 * (the spectral approach of RNNoise / DeepFilterNet, no ML weights).
 * Operates on WB_SAMPLE_RATE mono or stereo float audio in-place-ish
 * (writes the cleaned signal to `out`). */

typedef struct wb_isolate wb_isolate;

/* Create an isolation context. reduction 0..1 (how aggressively to suppress
 * below the noise floor), floor_db noise gate threshold (e.g. -40).
 * Returns NULL on alloc failure. */
wb_isolate *wb_isolate_create(float reduction, float floor_db);
void        wb_isolate_destroy(wb_isolate *iso);

/* Process `frames` samples of `in` (mono float, -1..1), write cleaned audio
 * to `out` (may alias `in`). Stateful across calls (noise floor adapts). */
void wb_isolate_process(wb_isolate *iso, const float *in, float *out, int frames);

/* Process an interleaved stereo buffer (2*frames samples). */
void wb_isolate_process_stereo(wb_isolate *iso, const float *in, float *out, int frames);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_WBUS_VOICE_ISOLATE_H */
