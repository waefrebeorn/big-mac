/* wbus_sf2.h — R074 hop 127 (G-SF062): minimal SoundFont 2 loader.
 * Presets -> resolved sample playback. Pure C11, stdlib only.
 */
#ifndef WUBUS_SF2_H
#define WUBUS_SF2_H

#include <stdint.h>
#include <stddef.h>
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_sf2 wb_sf2;

/* Load and parse an .sf2 file. Returns NULL on error. */
wb_sf2 *wb_sf2_load(const char *path);

int         wb_sf2_preset_count(const wb_sf2 *s);
const char *wb_sf2_preset_name(const wb_sf2 *s, int idx);

/* Render one MIDI note from a preset: pitch-shifted sample playback,
 * accumulated (+=) into interleaved stereo out. Returns frames written. */
uint32_t wb_sf2_render_note(const wb_sf2 *s, int preset, int pitch,
                            double dur_s, uint32_t sr,
                            wb_sample *out, uint8_t vel);

void wb_sf2_free(wb_sf2 *s);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_SF2_H */
