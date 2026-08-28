/* wb_midi_note_to_freq_stub.c — standalone object providing only
 * wb_midi_note_to_freq (needed by wb_fm.o's test harness link).
 * Also provides a minimal wb_unit_register stub (wb_fm.o calls it
 * during static init via wb_unit_ensure_fm).
 * Deliberately NOT including coreMIDI framework. */

#include <math.h>

double wb_midi_note_to_freq(int note) {
    return 440.0 * pow(2.0, (note - 69) / 12.0);
}

/* Minimal stub: wb_fm.o calls wb_unit_register(&u_fm_unit) at static
 * init time; the real registry lives in wb_unit.o which we aren't
 * linking here. The stub just swallows the call. */
void wb_unit_register(const void *u) { (void)u; }
