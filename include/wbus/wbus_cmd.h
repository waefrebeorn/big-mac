#ifndef WUBUS_WBUS_CMD_H
#define WUBUS_WBUS_CMD_H

/* Big Mac DAW — lock-free SPSC command queue (UI -> engine).
 * The audio (RT) thread never blocks, never allocates, never locks.
 * Fixed-size command ring; atomic head/tail indexes (C11 atomics).
 *
 * Producer = UI thread (single). Consumer = engine RT thread (single).
 */

#include <stdint.h>
#include <stdatomic.h>
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WB_CMD_QUEUE_CAP 4096

typedef enum {
    WB_CMD_PLAY = 1,
    WB_CMD_STOP,
    WB_CMD_SEEK,
    WB_CMD_SET_BPM,
    WB_CMD_SET_TRACK_VOL,
    WB_CMD_NOTE,
    WB_CMD_SET_INSERT_PARAM,
} wb_cmd_type;

typedef struct wb_cmd {
    uint32_t type;
    int64_t  i0, i1;    /* int payload */
    double   f0, f1;    /* float payload */
} wb_cmd;

typedef struct wb_cmd_queue {
    _Atomic uint64_t head;   /* producer writes at head, then bumps */
    _Atomic uint64_t tail;   /* consumer reads at tail, then bumps */
    wb_cmd ring[WB_CMD_QUEUE_CAP];
} wb_cmd_queue;

static inline void wb_cmd_queue_init(wb_cmd_queue *q) {
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
}

/* Producer (UI thread). Returns 1 if queued, 0 if full. */
static inline int wb_cmd_push(wb_cmd_queue *q, wb_cmd c) {
    uint64_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head - tail >= WB_CMD_QUEUE_CAP) return 0; /* full */
    q->ring[head % WB_CMD_QUEUE_CAP] = c;
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return 1;
}

/* Consumer (engine RT thread). Returns 1 if a cmd was read, 0 if empty. */
static inline int wb_cmd_pop(wb_cmd_queue *q, wb_cmd *out) {
    uint64_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail == head) return 0; /* empty */
    *out = q->ring[tail % WB_CMD_QUEUE_CAP];
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_CMD_H */
