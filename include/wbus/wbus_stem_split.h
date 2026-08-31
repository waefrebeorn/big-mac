/* wbus_stem_split.h — Intelligent 4-stem separation (R078-S).
 *
 * Separates a mixed audio signal into vocals, drums, bass, and other,
 * using multi-pass spectral decomposition (Ableton Live 12.3 style).
 *
 * Algorithm (pure DSP, no ML weights — deterministic, offline-capable):
 *   1. HPSS: harmonic (vocals+bass+other) vs percussive (drums)
 *   2. On harmonic: low-frequency extraction → bass vs mid/high
 *   3. On mid/high: spectral centroid + formant detection → vocals vs other
 *
 * Soft Wiener masking throughout for clean separation with minimal artifacts.
 * STFT: 4096 frame, 1024 hop, Hann window.
 *
 * Pure C11, zero third-party. */
#ifndef WBUS_STEM_SPLIT_H
#define WBUS_STEM_SPLIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Split a mono mix into 4 mono stems (vocals, drums, bass, other).
 * mix: input buffer of `frames` mono samples.
 * vocals, drums, bass, other: caller-allocated output buffers (frames each).
 * Returns 0 on success, -1 on error (null pointers, bad params). */
int wb_stem_split(const float *mix, uint32_t frames,
                  float *vocals, float *drums, float *bass, float *other);

/* Split a stereo mix into 4 stereo stems.
 * mix: interleaved stereo input (frames*2 samples).
 * Output buffers are planar stereo: e.g. vocals_l[i], vocals_r[i] for frame i.
 * Returns 0 on success, -1 on error. */
int wb_stem_split_4stem(const float *mix, uint32_t frames, uint32_t chn,
                        float *vocals_l, float *vocals_r,
                        float *drums_l, float *drums_r,
                        float *bass_l, float *bass_r,
                        float *other_l, float *other_r);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_STEM_SPLIT_H */