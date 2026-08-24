/* wb_cgi_bands.h — R073 hop 61: frequency-band audio-reactive CGI. */
#ifndef WBUS_CGI_BANDS_H
#define WBUS_CGI_BANDS_H

#include "wbus.h"

struct wb_anim;
/* Bake per-window band energy (FFT in [lo_hz,hi_hz)) into scale/spin keys.
 * Returns keys written or -1. */
int wb_cgi_band_pulse(struct wb_anim *a, int obj,
                      const wb_sample *audio, uint32_t frames,
                      uint32_t chn, double dur_secs,
                      float lo_hz, float hi_hz,
                      float base, float amount);
#endif
