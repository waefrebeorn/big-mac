#ifndef WBUS_WBUS_SPECTRAL_FX_H
#define WBUS_WBUS_SPECTRAL_FX_H

#include <stdint.h>
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Spectral effects (Ableton-style): resonator, blur, time-freeze.
 * All processing is mono (wb_sample). For stereo, create two instances.
 * Uses wb_fft.c internally for FFT/IFFT. Frame size 2048, 4x overlap. */

/* Effect types */
#define WB_SPECTRAL_RESONATOR  0
#define WB_SPECTRAL_BLUR       1
#define WB_SPECTRAL_TIME       2

/* Param IDs */
#define WB_SPECTRAL_PARAM_AMOUNT    0
#define WB_SPECTRAL_PARAM_FREQUENCY 1
#define WB_SPECTRAL_PARAM_DECAY     2
#define WB_SPECTRAL_PARAM_MIX       3

void *wb_spectral_fx_create(uint32_t sr);
void  wb_spectral_fx_destroy(void *sf);
void  wb_spectral_fx_set_type(void *sf, int type);    /* 0=resonator, 1=blur, 2=time */
void  wb_spectral_fx_set_param(void *sf, int param, float value);
void  wb_spectral_fx_process(void *sf, wb_sample *out, const wb_sample *in, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_WBUS_SPECTRAL_FX_H */