#ifndef WUBUS_WBUS_CLAP_H
#define WUBUS_WBUS_CLAP_H

/* Big Mac DAW — CLAP plugin host.
 * Loads .clap shared libraries via dlopen, finds the clap_entry symbol,
 * enumerates factory plugins, instantiates one, and bridges it to the wbus
 * plugin ABI so a third-party CLAP plugin runs inside the engine.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_clap_host wb_clap_host;

/* A loaded, instantiated CLAP plugin, exposed through the wbus plugin ABI.
 * (opaque; the engine treats it via the wbus vtable) */
typedef struct wb_clap_plugin wb_clap_plugin;

/* Create a host. Scans `search_path` (may be NULL -> default dirs) for
 * `.clap`/`.dylib`/.so files and indexes their descriptors. */
wb_clap_host *wb_clap_host_create(const char *search_path);

/* Count indexed plugins across all scanned libraries. */
uint32_t wb_clap_host_plugin_count(wb_clap_host *h);

/* Get the name+id of the plugin at `index` (for a browser list). */
int wb_clap_host_plugin_info(wb_clap_host *h, uint32_t index,
                             const char **name, const char **id);

/* Instantiate the plugin at `index` into a wbus-ABI instance at `sample_rate`.
 * Returns NULL on failure. Caller eventually calls wb_clap_plugin_destroy. */
wb_clap_plugin *wb_clap_plugin_create(wb_clap_host *h, uint32_t index,
                                      uint32_t sample_rate);

/* Process one block (interleaved stereo in->out, `frames` samples).
 * Returns 0 on success, nonzero on error. */
int wb_clap_plugin_process(wb_clap_plugin *p,
                           const float *inL, const float *inR,
                           float *outL, float *outR,
                           uint32_t frames);

void wb_clap_plugin_destroy(wb_clap_plugin *p);
void wb_clap_host_destroy(wb_clap_host *h);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_CLAP_H */
