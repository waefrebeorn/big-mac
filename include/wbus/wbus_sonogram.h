#ifndef WBUS_WBUS_SONOGRAM_H
#define WBUS_WBUS_SONOGRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* wb_sonogram — real-time spectrogram + waveform display (iZotope RX style).
 * STFT-based time-frequency visualization with log-frequency axis,
 * magnitude-to-color mapping (black→blue→green→yellow→red), and a
 * waveform overlay strip at the top of the render buffer. */

typedef struct wb_sonogram wb_sonogram;

/* Create a sonogram analyzer. sr = sample rate. width/height = RGBA buffer
 * dimensions for rendering. Returns NULL on OOM / bad params. */
wb_sonogram *wb_sonogram_create(uint32_t sr, int width, int height);

/* Destroy a sonogram analyzer and free all internal buffers. */
void wb_sonogram_destroy(wb_sonogram *sg);

/* Feed audio for analysis. audio = interleaved mono or stereo (we use L).
 * frames = number of sample frames. Returns 0 on success. */
int wb_sonogram_process(wb_sonogram *sg, const float *audio, uint32_t frames);

/* Render the current spectrogram into an RGBA buffer. The buffer must be
 * at least width*height*4 bytes. Waveform overlay is drawn in the top
 * ~15% of the height. Returns 0 on success. */
int wb_sonogram_render(wb_sonogram *sg, uint8_t *rgba_out, int width, int height);

/* Get the peak absolute sample value seen since last reset (0..1+). */
float wb_sonogram_get_peak(const wb_sonogram *sg);

/* Get the RMS of the last processed block (0..1). */
float wb_sonogram_get_rms(const wb_sonogram *sg);

/* Get the crest factor (peak/rms) of the last processed block (>=1.0). */
float wb_sonogram_get_crest_factor(const wb_sonogram *sg);

/* Get the spectral centroid in Hz from the most recent FFT frame.
 * Returns 0 if no FFT has been computed yet. */
float wb_sonogram_get_spectral_centroid(const wb_sonogram *sg);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_WBUS_SONOGRAM_H */