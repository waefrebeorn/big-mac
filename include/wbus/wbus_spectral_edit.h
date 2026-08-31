#ifndef WBUS_WBUS_SPECTRAL_EDIT_H
#define WBUS_WBUS_SPECTRAL_EDIT_H

#include <stdint.h>
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Spectral audio repair tools (iZotope RX style).
 * All functions process interleaved multi-channel audio in-place-capable
 * (separate in/out buffers). Return 0 on success, -1 on error. */

/* Spectral gating denoise. strength in [0,1]: 0 = no-op, 1 = max reduction.
 * Estimates noise floor from first 100ms, attenuates bins below threshold. */
int wb_spectral_denoise(const wb_sample *in, wb_sample *out, uint32_t frames,
                        uint32_t chn, float strength);

/* Declick: detect transient spikes via derivative threshold,
 * interpolate affected samples. threshold is the derivative magnitude
 * above which a sample is considered a click (e.g. 0.1–0.5). */
int wb_spectral_declick(const wb_sample *in, wb_sample *out, uint32_t frames,
                        uint32_t chn, float threshold);

/* Dehum: notch filter at hum_freq (typically 50 or 60 Hz) and harmonics
 * up to 1000 Hz. Uses STFT-domain spectral notching. */
int wb_spectral_dehum(const wb_sample *in, wb_sample *out, uint32_t frames,
                      uint32_t chn, float hum_freq);

/* Gain: simple dB gain applied in time domain. gain_db can be negative. */
int wb_spectral_gain(const wb_sample *in, wb_sample *out, uint32_t frames,
                     uint32_t chn, float gain_db);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_WBUS_SPECTRAL_EDIT_H */