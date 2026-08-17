#ifndef WUBUS_WBUS_VST3_H
#define WUBUS_WBUS_VST3_H

/* Big Mac DAW — VST3 plugin host C11 API.
 *
 * Pure C11 interface to the C++ VST3 host shim (src/wb_vst3_host.cpp).
 * The host shim owns VST3 SDK objects; this header exposes only C11 types
 * and functions that the C engine can call.
 *
 * Workflow:
 *   1. wb_vst3_scan(dir)  — discover .vst3 bundles in dir (or default dirs)
 *   2. wb_vst3_plugin_count() — how many were found
 *   3. wb_vst3_plugin_name(i) — human-readable name of i-th plugin
 *   4. wb_vst3_create(name, sr) — instantiate a plugin by name
 *   5. wb_vst3_set_sample_rate(inst, sr) — set SR (caches for process)
 *   6. wb_vst3_process(inst, inL, inR, outL, outR, n) — render one block
 *   7. wb_vst3_get_param/set_param(inst, idx, v) — parameter automation
 *   8. wb_vst3_get_info(inst, ...) — name/vendor/category
 *   9. wb_vst3_destroy(inst) — release
 *
 * VST3 plugin descriptor id convention: when a VST3 plugin is inserted into
 * a track slot, its id is prefixed with "vst3:" followed by the plugin's
 * name (or a stable id). The engine's insert chain dispatch (stage_effects)
 * checks for "vst3:" prefix and routes to the VST3 host for process().
 * Currently the engine stores the VST3 instance pointer via a unit-slot
 * mapping table (wb_vst3_slot_map) that is populated by wb_vst3_create.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* Scan a directory for VST3 plugin bundles (.vst3 files/dirs).
 * Common directories: /Library/Audio/Plug-Ins/VST3 (system),
 * ~/Library/Audio/Plug-Ins/VST3 (user). Also tries ./vst3_plugins (dev).
 * Returns 1 if any plugins were found, 0 otherwise. */
int wb_vst3_scan(const char* dir_path);

/* Number of plugins found by the last scan. */
int wb_vst3_plugin_count(void);

/* Human-readable name of the i-th plugin (0-based). NULL if out of range.
 * Caller must not free. The name can be used in wb_vst3_create(). */
const char* wb_vst3_plugin_name(int i);

/* Create a VST3 plugin instance by name (must match a name from scan).
 * sample_rate: the sample rate the plugin will process at.
 * Returns opaque handle, or NULL on failure. */
void* wb_vst3_create(const char* name, uint32_t sample_rate);

/* Release a VST3 plugin instance. */
void wb_vst3_destroy(void* inst);

/* Process one block of audio through a VST3 plugin.
 * inL/inR: input buffers (n frames, stereo)
 * outL/outR: output buffers (n frames, stereo) — written by the plugin
 * Returns 0 on success, -1 on error. */
int wb_vst3_process(void* inst, const float* inL, const float* inR,
                    float* outL, float* outR, uint32_t n);

/* Get a plugin parameter value (normalized 0..1). param_index is the
 * plugin's internal parameter index. Returns the cached value. */
float wb_vst3_get_param(void* inst, int param_index);

/* Number of parameters the plugin exposes. 0 if no controller. */
int   wb_vst3_param_count(void* inst);

/* Copy the title of parameter `idx` into `out` (sized `outsz`).
 * Returns characters written (excl. NUL), or -1 on error. */
int   wb_vst3_param_name(void* inst, int idx, char* out, int outsz);

/* Set a plugin parameter value (normalized 0..1). Caches the value and
 * attempts to push to the VST3 controller. Returns 0 on success. */
int wb_vst3_set_param(void* inst, int param_index, float value);

/* Get plugin metadata: name, vendor, category. Caller provides buffers. */
int wb_vst3_get_info(void* inst, char* name, int name_sz,
                     char* vendor, int vendor_sz,
                     char* category, int cat_sz);

/* Get the last error message (if any). NULL if no error. Caller must not free. */
const char* wb_vst3_error(void);

/* Look up the live VST3 instance bound to a track+slot (NULL if none).
 * The engine stores instances in a track/slot map; the DAW uses this to
 * enumerate and edit plugin parameters. Caller must not free the handle. */
void* wb_vst3_slot_get(int track, int slot);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_VST3_H */
