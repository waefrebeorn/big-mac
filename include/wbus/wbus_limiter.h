/* wb_limiter.h — R073 hop 37: lookahead brickwall limiter (two-pass). */
#ifndef WBUS_LIMITER_H
#define WBUS_LIMITER_H

#include "wbus.h"

typedef struct wb_limiter wb_limiter;

wb_limiter *wb_limiter_create(double sr, double lookahead_ms, float ceiling);
/* in-place, interleaved stereo; returns 0 or -1 */
int  wb_limiter_process(wb_limiter *lm, wb_sample *buf, uint32_t frames);
void wb_limiter_destroy(wb_limiter *lm);
#endif
