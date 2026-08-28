/* wb_ringbuf.h — lock-free single-producer/single-consumer ring buffer.
 *
 * R077: Cache-line-padded, wait-free ring buffer for audio threading.
 * Used for: MIDI command queue, inter-thread audio passing, parameter updates.
 *
 * Key optimizations:
 *   - Cache-line padding (64 bytes) to prevent false sharing
 *   - Power-of-2 size for fast modulo (bitwise AND)
 *   - Memory ordering: acquire/release (not full barrier)
 *   - No locks, no syscalls, no malloc in hot path
 *
 * Pure C11, atomics. Header-only.
 */

#ifndef WB_RINGBUF_H
#define WB_RINGBUF_H

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WB_CACHE_LINE_SIZE 64

/* Lock-free SPSC ring buffer.
 * Size MUST be a power of 2.
 * One writer thread, one reader thread. */
typedef struct {
    /* Cache-line separated read/write indices to prevent false sharing */
    alignas(WB_CACHE_LINE_SIZE) atomic_size_t head;     /* write index (producer) */
    alignas(WB_CACHE_LINE_SIZE) atomic_size_t tail;     /* read index (consumer) */

    size_t capacity;    /* Must be power of 2 */
    size_t mask;        /* capacity - 1, for fast modulo */
    size_t elem_size;   /* Size of each element in bytes */
    uint8_t *data;      /* Raw buffer */
} wb_ringbuf;

/* Create a ring buffer. capacity must be power of 2. */
static inline wb_ringbuf* wb_ringbuf_create(size_t capacity, size_t elem_size) {
    /* Ensure power of 2 */
    if (capacity & (capacity - 1)) return NULL;

    wb_ringbuf *rb = (wb_ringbuf *)aligned_alloc(WB_CACHE_LINE_SIZE, sizeof(wb_ringbuf));
    if (!rb) return NULL;
    memset(rb, 0, sizeof(*rb));

    rb->capacity = capacity;
    rb->mask = capacity - 1;
    rb->elem_size = elem_size;
    rb->data = (uint8_t *)calloc(capacity, elem_size);

    if (!rb->data) { free(rb); return NULL; }

    atomic_store_explicit(&rb->head, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, 0, memory_order_relaxed);

    return rb;
}

static inline void wb_ringbuf_destroy(wb_ringbuf *rb) {
    if (rb) { free(rb->data); free(rb); }
}

/* Returns number of elements available to read */
static inline size_t wb_ringbuf_read_available(wb_ringbuf *rb) {
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    return (head - tail) & rb->mask;
}

/* Returns number of free slots for writing */
static inline size_t wb_ringbuf_write_available(wb_ringbuf *rb) {
    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    return (rb->capacity - 1 - (head - tail)) & rb->mask;
}

/* Write one element. Returns 1 on success, 0 if full. */
static inline int wb_ringbuf_write(wb_ringbuf *rb, const void *elem) {
    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t next = (head + 1) & rb->mask;

    /* Check if full */
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    if (next == tail) return 0;  /* Full */

    /* Write data */
    memcpy(rb->data + head * rb->elem_size, elem, rb->elem_size);

    /* Update head (release: ensure data is visible before index update) */
    atomic_store_explicit(&rb->head, next, memory_order_release);
    return 1;
}

/* Read one element. Returns 1 on success, 0 if empty. */
static inline int wb_ringbuf_read(wb_ringbuf *rb, void *elem) {
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);

    /* Check if empty */
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    if (tail == head) return 0;  /* Empty */

    /* Read data */
    memcpy(elem, rb->data + tail * rb->elem_size, rb->elem_size);

    /* Update tail */
    atomic_store_explicit(&rb->tail, (tail + 1) & rb->mask, memory_order_release);
    return 1;
}

/* Typed ring buffer for float audio samples */
typedef wb_ringbuf wb_audio_ringbuf;

static inline wb_audio_ringbuf* wb_audio_ringbuf_create(size_t capacity) {
    return wb_ringbuf_create(capacity, sizeof(float));
}

static inline int wb_audio_ringbuf_write_sample(wb_audio_ringbuf *rb, float sample) {
    return wb_ringbuf_write(rb, &sample);
}

static inline int wb_audio_ringbuf_read_sample(wb_audio_ringbuf *rb, float *sample) {
    return wb_ringbuf_read(rb, sample);
}

#ifdef __cplusplus
}
#endif

#endif /* WB_RINGBUF_H */
