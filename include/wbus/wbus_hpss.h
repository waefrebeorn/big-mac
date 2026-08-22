/* wbus_hpss.h — Harmonic-Percussive Source Separation (R020-A).
 *
 * Classic two-stage HPSS (Driedger & Soros, "Two-Stage Algorithm for
 * Music Source Separation"): in the STFT magnitude domain, harmonics are
 * horizontal lines (stable across time at a frequency) and transients are
 * vertical lines (brief across frequency). A frequency-axis median suppresses
 * transients (keeps harmonic); a time-axis median on the residual isolates
 * percussion. Each component is resynthesized with the original phase.
 *
 * Pure C11, no ML weights, runs on the dual-core iMac. This gives the audio
 * editor a stem split: melody/bass (harmonic) vs drums (percussive). */
#ifndef WBUS_HPSS_H
#define WBUS_HPSS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct wb_hpss wb_hpss;

/* frame_size: FFT size (power of two, e.g. 2048). sr: sample rate.
 * h_len / p_len: median filter half-windows (bins for harmonic, frames for
 * percussive). Defaults of ~31 give a solid split. */
wb_hpss *wb_hpss_create(int frame_size, float sample_rate, int h_len, int p_len);
void      wb_hpss_destroy(wb_hpss *h);

/* Separate one channel. in/out are `frames` mono floats. harmonic and
 * percussive must be caller-allocated (frames each). Returns 0 on success.
 * harmonic + percussive reconstructs (approximately) the input. */
int wb_hpss_separate(wb_hpss *h, const float *in, uint32_t frames,
                     float *harmonic, float *percussive);


#ifdef __cplusplus
}
#endif
#endif /* WBUS_HPSS_H */
