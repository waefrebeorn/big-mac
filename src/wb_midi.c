/* wb_midi.c — MIDI note mapping and utility. Minimal for now: standard MIDI
 * note-to-frequency and channel helpers. External MIDI in is wired later.
 */

#include <math.h>
#include "wbus.h"

/* MIDI note -> frequency (Hz), A4=440, equal temperament. */
double wb_midi_note_to_freq(int note) {
    return 440.0 * pow(2.0, (note - 69) / 12.0);
}

/* Map a CC/knob 0..1 range to a target range. */
float wb_midi_map01(float v01, float min, float max) {
    return min + (max - min) * (v01 < 0 ? 0 : (v01 > 1 ? 1 : v01));
}
