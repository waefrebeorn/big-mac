#ifndef WUBUS_WBUS_MIDI_H
#define WUBUS_WBUS_MIDI_H

/* Big Mac DAW — portable MIDI input layer.
 * Abstraction over platform MIDI (CoreMIDI on macOS; the platform's own C
 * API, same treatment as CoreAudio/SDL). Enables plugging in controllers:
 * Ableton Launchpad, M-Audio keys, drum pads, etc.
 *
 * Pattern mirrors wb_backend: portable interface + platform implementation.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_midi wb_midi;

/* A MIDI event received from a controller. */
typedef struct wb_midi_event {
    uint8_t status;   /* 0x80 noteoff, 0x90 noteon, 0xB0 CC, 0xE0 bend */
    uint8_t data1;    /* note/CC number */
    uint8_t data2;    /* velocity/value */
} wb_midi_event;

/* Enumerate available MIDI input devices.
 * Fills `names` with up to `max` device names (caller-provided buffers of
 * `buf_size` bytes). Returns the number found. */
int wb_midi_enumerate(char (*names)[64], int max, int *out_count);

/* Open a MIDI input device by exact name (e.g. "Launchpad MK2").
 * Returns a handle, or NULL on failure. `on_event` is called from a MIDI
 * thread for each incoming event; `userdata` is passed through. */
wb_midi *wb_midi_open(const char *name,
                      void (*on_event)(wb_midi_event ev, void *userdata),
                      void *userdata);

/* Open the first device whose name CONTAINS `substr` (case-insensitive).
 * Convenience for autodetect ("Launchpad", "M-Audio", "AKAI"...). */
wb_midi *wb_midi_open_contains(const char *substr,
                               void (*on_event)(wb_midi_event ev, void *userdata),
                               void *userdata);

void wb_midi_close(wb_midi *m);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_MIDI_H */
