#ifndef WBUS_WBUS_CONV_H
#define WBUS_WBUS_CONV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convolution reverb (non-uniform partitioned overlap-save).
 * G1 [R075]: real-time IR convolution on dual-core C11. */

typedef struct wb_conv_inst wb_conv_inst;

/* Create/destroy */
wb_conv_inst *wb_conv_create(uint32_t sr);
void          wb_conv_destroy(wb_conv_inst *c);

/* Load a mono float impulse response. block_size = host buffer size
 * (e.g. 128). Returns 0 on success, negative on error. */
int wb_conv_load_ir(wb_conv_inst *c, const float *ir_mono, int ir_len, int block_size);

/* Process a stereo block. inL/inR and outL/outR are length n. */
void wb_conv_process(wb_conv_inst *c, const float *inL, const float *inR,
                     float *outL, float *outR, int n);

/* Parameters */
void wb_conv_set_mix(wb_conv_inst *c, float mix);   /* 0=dry, 1=wet */
void wb_conv_set_gain(wb_conv_inst *c, float gain);

/* Introspection */
int wb_conv_get_partitions(wb_conv_inst *c);
int wb_conv_get_part_len(wb_conv_inst *c, int idx);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_WBUS_CONV_H */
