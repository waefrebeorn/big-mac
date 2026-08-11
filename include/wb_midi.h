/*
 * wb_midi.h — hand-written Standard MIDI File writer (no third party)
 */
#ifndef WB_MIDI_H
#define WB_MIDI_H

/* Write a MIDI file (format 1, one track) with note-on/off events.
 * notes: array of {start_beats, duration_beats, midi_note, velocity}.
 * Returns 0 on success. */
typedef struct {
    double start_beats;
    double duration_beats;
    int note;        /* 0-127 */
    int velocity;    /* 1-127 */
} wb_midi_note_t;

int wb_midi_write(const char *path, const wb_midi_note_t *notes, int nnotes,
                  int bpm, int ppq /* pulses per quarter */);

#endif /* WB_MIDI_H */
