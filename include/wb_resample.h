/*
 * wb_resample.h — windowed-sinc (Kaiser) resampler, stolen from wuburvc
 * (the other agent's RVC engine, pure DSP — attribution: WaefreBeorn-UMV3)
 */
#ifndef WB_RESAMPLE_H
#define WB_RESAMPLE_H

/* Resample float mono PCM. out must hold n_out = round(n * out_sr / in_sr).
 * Returns n_out, or -1 on bad args. */
int wb_resample_sinc(const float *in, int n, int in_sr, int out_sr, float *out);

#endif /* WB_RESAMPLE_H */
