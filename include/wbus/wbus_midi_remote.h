#ifndef WUBUS_WBUS_MIDI_REMOTE_H
#define WUBUS_WBUS_MIDI_REMOTE_H

/* Big Mac DAW — MIDI remote control mapping.
 * Maps MIDI CC messages to track parameters (Ableton MIDI remote script style).
 * Each CC number (0..127) can be mapped to one (track, parameter) pair.
 * When a CC is received, the normalized value (0..1) is applied to the target.
 *
 * Parameter mapping: 0=volume 1=pan 2=mute 3=solo 4=sendA 5=sendB
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_midi_remote wb_midi_remote;

/* Create / destroy a remote mapping context. */
wb_midi_remote *wb_midi_remote_create(void);
void wb_midi_remote_destroy(wb_midi_remote *mr);

/* Load/save mappings from/to a JSON file. Returns 0 on success, -1 on error. */
int wb_midi_remote_load(wb_midi_remote *mr, const char *path);
int wb_midi_remote_save(wb_midi_remote *mr, const char *path);

/* Add a mapping: cc (0..127) -> (track, param). Returns 0 ok, -1 on error. */
int wb_midi_remote_add_map(wb_midi_remote *mr, int cc, int track, int param);

/* Remove the mapping for a given CC. Returns 0 ok, -1 if not found. */
int wb_midi_remote_remove_map(wb_midi_remote *mr, int cc);

/* Process a CC: look up mapping, apply normalized value (0..1) to target.
 * Returns 0 if a mapping was found and applied, -1 if no mapping for this CC. */
int wb_midi_remote_process(wb_midi_remote *mr, int cc, float value);

/* Get the mapped (track, param) for a CC. Returns 0 if found, -1 if not. */
int wb_midi_remote_get_mapped_param(wb_midi_remote *mr, int cc, int *track, int *param);

/* Clear all mappings. */
int wb_midi_remote_clear(wb_midi_remote *mr);

/* Return the number of active mappings. */
int wb_midi_remote_count(wb_midi_remote *mr);

/* Named parameter IDs (for readability at call sites). */
#define WB_MIDI_REMOTE_PARAM_VOLUME 0
#define WB_MIDI_REMOTE_PARAM_PAN    1
#define WB_MIDI_REMOTE_PARAM_MUTE   2
#define WB_MIDI_REMOTE_PARAM_SOLO   3
#define WB_MIDI_REMOTE_PARAM_SENDA  4
#define WB_MIDI_REMOTE_PARAM_SENDB  5

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_MIDI_REMOTE_H */