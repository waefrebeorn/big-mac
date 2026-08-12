#ifndef WUBUS_WBUS_BACKEND_H
#define WUBUS_WBUS_BACKEND_H

/* Big Mac DAW — audio backend interface.
 * A backend pulls blocks from the engine and pushes them to hardware (or a
 * file). CoreAudio drives the realtime path on macOS; a file backend renders
 * offline. Both share the exact same engine render path (oracle parity).
 */

#include <stdint.h>
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_backend wb_backend;

typedef struct wb_backend_info {
    char     name[64];
    uint32_t sample_rate;
    uint8_t  channels;
} wb_backend_info;

/* Create a realtime CoreAudio backend. Returns NULL on failure. */
wb_backend *wb_backend_coreaudio_create(wb_engine *e, uint32_t sample_rate);

/* Create an offline WAV-rendering backend (headless; no hardware). */
wb_backend *wb_backend_file_create(wb_engine *e, const char *path, uint32_t sample_rate);

void        wb_backend_destroy(wb_backend *b);
const wb_backend_info *wb_backend_get_info(const wb_backend *b);
int         wb_backend_start(wb_backend *b);
void        wb_backend_stop(wb_backend *b);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_BACKEND_H */
