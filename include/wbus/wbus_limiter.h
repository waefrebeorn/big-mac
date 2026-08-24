/* wb_limiter.h — R073 hop 37: lookahead brickwall limiter (two-pass). */
#ifndef WBUS_LIMITER_H
#define WBUS_LIMITER_H

#include "wbus.h"

typedef struct wb_limiter wb_limiter;

wb_limiter *wb_limiter_create(double sr, double lookahead_ms, float ceiling);
/* in-place, interleaved stereo; returns 0 or -1 */
int  wb_limiter_process(wb_limiter *lm, wb_sample *buf, uint32_t frames);
void wb_limiter_destroy(wb_limiter *lm);

/* ---- R073 hop 39: streaming (RT) variant ---- */
typedef struct wb_stream_limiter wb_stream_limiter;
wb_stream_limiter *wb_stream_limiter_create(double sr, double lookahead_ms,
                                            float ceiling);
void wb_stream_limiter_destroy(wb_stream_limiter *lm);
float wb_stream_limiter_gain(const wb_stream_limiter *lm);
/* processes exactly one interleaved stereo frame */
void wb_stream_limiter_frame(wb_stream_limiter *lm, wb_sample *frame);
#endif
