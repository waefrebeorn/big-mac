/* src/wb_score.c — MIDI to notation converter (Cubase score editor style).
 * Converts MIDI pitches to staff position, note names, accidentals, and
 * renders an ASCII-art 5-line staff. Pure C11, zero third-party.
 */
#include <stdio.h>
#include <string.h>
#include "wbus.h"

/* Note names indexed by pitch class (0=C, 1=C#, ..., 11=B). */
static const char *const k_note_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

/* Pitch classes that are accidentals (sharps / black keys). */
static const int k_accidental_pcs[5] = {1, 3, 6, 8, 10}; /* C#, D#, F#, G#, A# */

/* Convert a MIDI pitch to a note name in `name_out` (e.g. "C4", "F#5"),
 * the octave in `octave_out`, and return 0 on success, -1 on bad input.
 * `cap` is the buffer capacity (must be >= 4 for "C4\0", >= 5 for "F#5\0"). */
int wb_score_note_to_staff(int midi_pitch, char *name_out, int cap, int *octave_out) {
    if (midi_pitch < 0 || midi_pitch > 127) return -1;
    if (!name_out || cap <= 0) return -1;

    int pc = midi_pitch % 12;
    /* MIDI 60 = C4, so octave = (pitch / 12) - 1.
     * This gives: 0..11 -> -1, 12..23 -> 0, ..., 60..71 -> 4, etc. */
    int octave = (midi_pitch / 12) - 1;
    if (octave_out) *octave_out = octave;

    const char *name = k_note_names[pc];
    int len = (int)snprintf(name_out, (size_t)cap, "%s%d", name, octave);
    if (len < 0 || len >= cap) return -1;
    return 0;
}

/* Return the staff line/space position for a MIDI pitch in treble clef.
 * Middle C (MIDI 60) = 0. Each whole step (diatonic) = 1 line position.
 * Each semitone = 0.5 line position. We use the diatonic scale degree
 * relative to C4 for clean staff positioning.
 *
 * Treble clef: lines bottom-to-top are E4(64), G4(67), B4(71), D5(74), F5(77).
 * Spaces: F4(65), A4(69), C5(72), E5(75).
 * Middle C (60) sits on a ledger line BELOW the staff = position 0.
 *
 * We compute: each natural note step from C4 = +1 position.
 * C=0, D=1, E=2, F=3, G=4, A=5, B=6 per octave.
 * Position = (octave - 4) * 7 + degree_in_octave.
 */
/* Compatibility alias */
int wb_score_staff_position(int midi_pitch) {
    return wb_score_pitch_to_line(midi_pitch);
}

int wb_score_pitch_to_line(int midi_pitch) {
    if (midi_pitch < 0) midi_pitch = 0;
    if (midi_pitch > 127) midi_pitch = 127;

    int pc = midi_pitch % 12;
    int octave = (midi_pitch / 12) - 1;

    /* Diatonic degree: map pitch class to scale degree (C=0, D=1, E=2, F=3, G=4, A=5, B=6).
     * For non-diatonic (sharps/flats), round to the lower natural degree. */
    static const int degree_table[12] = {
        0,  /* C  */
        0,  /* C# -> C */
        1,  /* D  */
        1,  /* D# -> D */
        2,  /* E  */
        3,  /* F  */
        3,  /* F# -> F */
        4,  /* G  */
        4,  /* G# -> G */
        5,  /* A  */
        5,  /* A# -> A */
        6   /* B  */
    };

    int degree = degree_table[pc];
    return (octave - 4) * 7 + degree;
}

/* Return 1 if the MIDI pitch is an accidental (black key / sharp), else 0. */
int wb_score_is_accidental(int midi_pitch) {
    if (midi_pitch < 0 || midi_pitch > 127) return 0;
    int pc = midi_pitch % 12;
    for (int i = 0; i < 5; i++) {
        if (k_accidental_pcs[i] == pc) return 1;
    }
    return 0;
}

/* Return 1 if the MIDI pitch is a black key on the keyboard, else 0.
 * (Same as accidental for our purposes — black keys = sharps.) */
int wb_score_is_black_key(int midi_pitch) {
    return wb_score_is_accidental(midi_pitch);
}

/* Render a measure of notes as ASCII-art staff notation.
 * The staff spans 5 lines (E4, G4, B4, D5, F5) with notes placed
 * above/below. Notes are rendered as 'o' on lines/spaces, with '#'
 * prefix for accidentals. Ledger lines are drawn for notes outside
 * the staff. Returns the number of characters written, or -1 on error.
 *
 * The output is a multi-line string (rows separated by '\n') showing:
 *   - A 5-line staff
 *   - Notes positioned vertically by pitch
 *   - Note names below each note position
 */
int wb_score_render_measure(wb_note *notes, int note_count, char *text_out, int cap) {
    if (!text_out || cap <= 0) return -1;
    if (!notes || note_count <= 0) {
        /* Empty measure — just draw the staff */
        text_out[0] = '\0';
        return 0;
    }

    /* Determine pitch range to size the rendering. */
    int min_line = 1000, max_line = -1000;
    for (int i = 0; i < note_count; i++) {
        int line = wb_score_pitch_to_line(notes[i].pitch);
        if (line < min_line) min_line = line;
        if (line > max_line) max_line = line;
    }

    /* Staff lines in our coordinate system:
     * E4=2, G4=4, B4=6, D5=8, F5=10 (positions of the 5 lines).
     * We render from max_line down to min_line.
     * Each line position is a half-step; we render every other as a staff line.
     */

    /* Build a map: for each note, which column it occupies.
     * Space notes out with 4-char columns. */
    int cols = note_count * 4;
    if (cols < 20) cols = 20; /* minimum staff width */

    /* Total rows: one row per line position from max to min. */
    int min_render = min_line - (min_line < 0 ? 2 : 0);
    int max_render = max_line + (max_line > 10 ? 2 : 0);
    /* Ensure staff lines (2,4,6,8,10) are always visible */
    if (min_render > 0) min_render = 0;
    if (max_render < 12) max_render = 12;

    int nrows = max_render - min_render + 1;

    /* Allocate a 2D grid. Use a stack buffer for reasonable sizes. */
    /* Max grid: 30 rows x 128 cols = 3840 chars — fine on stack. */
    if (nrows > 64 || cols > 256) {
        cols = 256;
        nrows = 64;
    }

    char grid[64][256];
    /* Initialize with spaces */
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < cols; c++) {
            grid[r][c] = ' ';
        }
    }

    /* Draw staff lines at positions 2,4,6,8,10 (E4,G4,B4,D5,F5). */
    int staff_lines[5] = {2, 4, 6, 8, 10};
    for (int s = 0; s < 5; s++) {
        int line_pos = staff_lines[s];
        int row = max_render - line_pos;
        if (row >= 0 && row < nrows) {
            for (int c = 0; c < cols; c++) {
                grid[row][c] = '-';
            }
        }
    }

    /* Place notes on the grid. */
    for (int i = 0; i < note_count; i++) {
        int pitch = notes[i].pitch;
        int line = wb_score_pitch_to_line(pitch);
        int row = max_render - line;
        int col = 2 + i * 4; /* column offset for this note */

        if (row < 0 || row >= nrows) continue;
        if (col >= cols) break;

        /* Check if this row is a staff line. */
        int is_staff_line = 0;
        for (int s = 0; s < 5; s++) {
            if (staff_lines[s] == line) { is_staff_line = 1; break; }
        }

        /* Draw ledger line if outside staff and not on a staff line. */
        if ((line < 2 || line > 10) && !is_staff_line) {
            if (col - 1 >= 0) grid[row][col - 1] = '-';
            grid[row][col] = '-';
            if (col + 1 < cols) grid[row][col + 1] = '-';
        }

        /* Place note head. If accidental, show '#' before the note. */
        char note_sym = 'o';
        if (wb_score_is_accidental(pitch)) {
            if (col - 1 >= 0) grid[row][col - 1] = '#';
        }
        grid[row][col] = note_sym;
    }

    /* Convert grid to output string. */
    int pos = 0;
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < cols; c++) {
            if (pos >= cap - 2) goto done;
            text_out[pos++] = grid[r][c];
        }
        /* Trim trailing spaces on each row. */
        while (pos > 0 && text_out[pos - 1] == ' ') pos--;
        if (pos >= cap - 2) goto done;
        text_out[pos++] = '\n';
    }

done:
    text_out[pos] = '\0';
    return pos;
}

/* Convenience: just get the note name (e.g. "C4", "F#5"). */
int wb_score_note_name(int midi_pitch, char *out, int cap) {
    int octave;
    return wb_score_note_to_staff(midi_pitch, out, cap, &octave);
}