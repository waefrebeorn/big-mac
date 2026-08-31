#ifndef WUBUS_WBUS_AI_MIX_H
#define WUBUS_WBUS_AI_MIX_H

/* wbus_ai_mix.h — AI-assisted mixing tools (Logic Mastering Assistant style).
 * Pure C11, zero third-party. Reuses wb_fft.c (spectrum) and wb_lufs.c
 * (K-weighted loudness) for the heavy lifting.
 *
 * Functions:
 *   analyze      — RMS, peak, LUFS, crest factor, spectral centroid
 *   auto_eq      — 8-band octave analyser + target-curve matching
 *   auto_level   — loudness-normalise to a target LUFS
 *   suggest_pan  — stereo-field placement based on spectral similarity
 *   de_ess       — sibilance detection + HF attenuation
 */

#include <stdint.h>
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 on success, negative on error.
 * All out-params are written if non-NULL. */
int wb_ai_mix_analyze(const wb_sample *audio, uint32_t frames, uint32_t sr,
                      float *rms_out, float *peak_out,
                      float *loudness_lufs_out, float *crest_factor_out,
                      float *spectral_centroid_out);

/* Analyse spectrum in 8 octave bands and write per-band gain suggestions
 * (in dB) to suggested_gains_db[0..num_bands-1].  Applies the suggested
 * EQ to produce 'out'.  num_bands is clamped to a maximum of 8.
 * Returns 0 on success. */
int wb_ai_mix_auto_eq(const wb_sample *in, wb_sample *out,
                      uint32_t frames, uint32_t sr,
                      float *suggested_gains_db, int num_bands);

/* Measure LUFS of 'in' and scale to reach target_lufs (e.g. -14.0 for
 * Spotify/YouTube Loudness Normalisation).  Writes gain-adjusted output
 * to 'out'.  Returns 0 on success. */
int wb_ai_mix_auto_level(const wb_sample *in, wb_sample *out,
                         uint32_t frames, uint32_t sr,
                         float target_lufs);

/* Suggest pan positions (-1.0 left, +1.0 right) for track_count tracks.
 * spectral_centroids[i] is the centroid (Hz) for track i; tracks with
 * similar centroids are spread apart to reduce masking.
 * Suggested values are clamped to [-1.0, 1.0].
 * Returns 0 on success. */
int wb_ai_mix_suggest_pan(uint32_t track_count,
                          const float *spectral_centroids,
                          float *suggested_pans);

/* Detect sibilance (HF energy ratio) and attenuate above threshold_db.
 * Returns 0 on success. */
int wb_ai_mix_de_ess(const wb_sample *in, wb_sample *out,
                     uint32_t frames, uint32_t sr,
                     float threshold_db);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_AI_MIX_H */
