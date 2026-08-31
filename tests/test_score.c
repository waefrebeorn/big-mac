/* tests/test_score.c — headless test of the MIDI-to-notation converter.
 * Verifies note naming, octave calculation, staff position, accidental
 * detection, black key detection, and measure rendering.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus.h"

static int failures = 0;
static int checks = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

int main(void) {
    char name_buf[16];
    int octave_out;

    /* ---- 1. Note name conversion ---- */
    printf("== Note name conversion ==\n");

    CHECK(wb_score_note_to_staff(60, name_buf, sizeof(name_buf), &octave_out) == 0,
          "wb_score_note_to_staff(60,...) returns success");
    CHECK(strcmp(name_buf, "C4") == 0, "MIDI 60 = C4 (middle C)");

    CHECK(wb_score_note_to_staff(69, name_buf, sizeof(name_buf), &octave_out) == 0,
          "wb_score_note_to_staff(69,...) returns success");
    CHECK(strcmp(name_buf, "A4") == 0, "MIDI 69 = A4 (concert A)");

    CHECK(wb_score_note_to_staff(48, name_buf, sizeof(name_buf), &octave_out) == 0,
          "wb_score_note_to_staff(48,...) returns success");
    CHECK(strcmp(name_buf, "C3") == 0, "MIDI 48 = C3 (one octave below middle C)");

    CHECK(wb_score_note_to_staff(72, name_buf, sizeof(name_buf), &octave_out) == 0,
          "wb_score_note_to_staff(72,...) returns success");
    CHECK(strcmp(name_buf, "C5") == 0, "MIDI 72 = C5 (one octave above middle C)");

    CHECK(wb_score_note_to_staff(61, name_buf, sizeof(name_buf), &octave_out) == 0,
          "wb_score_note_to_staff(61,...) returns success");
    CHECK(strcmp(name_buf, "C#4") == 0, "MIDI 61 = C#4 (sharp note name)");

    CHECK(wb_score_note_to_staff(66, name_buf, sizeof(name_buf), &octave_out) == 0,
          "wb_score_note_to_staff(66,...) returns success");
    CHECK(strcmp(name_buf, "F#4") == 0, "MIDI 66 = F#4");

    /* ---- 2. Octave calculation ---- */
    printf("\n== Octave calculation ==\n");

    wb_score_note_to_staff(60, name_buf, sizeof(name_buf), &octave_out);
    CHECK(octave_out == 4, "MIDI 60 octave = 4");

    wb_score_note_to_staff(48, name_buf, sizeof(name_buf), &octave_out);
    CHECK(octave_out == 3, "MIDI 48 octave = 3");

    wb_score_note_to_staff(72, name_buf, sizeof(name_buf), &octave_out);
    CHECK(octave_out == 5, "MIDI 72 octave = 5");

    wb_score_note_to_staff(0, name_buf, sizeof(name_buf), &octave_out);
    CHECK(octave_out == -1, "MIDI 0 octave = -1");

    wb_score_note_to_staff(127, name_buf, sizeof(name_buf), &octave_out);
    CHECK(octave_out == 9, "MIDI 127 octave = 9");

    /* ---- 3. Staff position (middle C = 0) ---- */
    printf("\n== Staff position ==\n");

    CHECK(wb_score_pitch_to_line(60) == 0, "Middle C (60) = staff position 0");

    /* D4 = 62, one whole step above C4 */
    CHECK(wb_score_pitch_to_line(62) == 1, "D4 (62) = staff position +1");

    /* E4 = 64, on the first staff line */
    CHECK(wb_score_pitch_to_line(64) == 2, "E4 (64) = staff position +2 (first line)");

    /* C3 = 48, well below the staff */
    CHECK(wb_score_pitch_to_line(48) == -7, "C3 (48) = staff position -7");

    /* C5 = 72, above the staff */
    CHECK(wb_score_pitch_to_line(72) == 7, "C5 (72) = staff position +7");

    /* ---- 4. Accidental detection ---- */
    printf("\n== Accidental detection ==\n");

    CHECK(wb_score_is_accidental(60) == 0, "C4 (60) is NOT an accidental");
    CHECK(wb_score_is_accidental(61) == 1, "C#4 (61) IS an accidental");
    CHECK(wb_score_is_accidental(62) == 0, "D4 (62) is NOT an accidental");
    CHECK(wb_score_is_accidental(63) == 1, "D#4 (63) IS an accidental");
    CHECK(wb_score_is_accidental(64) == 0, "E4 (64) is NOT an accidental");
    CHECK(wb_score_is_accidental(65) == 0, "F4 (65) is NOT an accidental");
    CHECK(wb_score_is_accidental(66) == 1, "F#4 (66) IS an accidental");
    CHECK(wb_score_is_accidental(67) == 0, "G4 (67) is NOT an accidental");
    CHECK(wb_score_is_accidental(68) == 1, "G#4 (68) IS an accidental");
    CHECK(wb_score_is_accidental(69) == 0, "A4 (69) is NOT an accidental");
    CHECK(wb_score_is_accidental(70) == 1, "A#4 (70) IS an accidental");
    CHECK(wb_score_is_accidental(71) == 0, "B4 (71) is NOT an accidental");

    /* ---- 5. Black key detection ---- */
    printf("\n== Black key detection ==\n");

    CHECK(wb_score_is_black_key(60) == 0, "C4 (60) is NOT a black key");
    CHECK(wb_score_is_black_key(61) == 1, "C#4 (61) IS a black key");
    CHECK(wb_score_is_black_key(63) == 1, "D#4 (63) IS a black key");
    CHECK(wb_score_is_black_key(66) == 1, "F#4 (66) IS a black key");
    CHECK(wb_score_is_black_key(68) == 1, "G#4 (68) IS a black key");
    CHECK(wb_score_is_black_key(70) == 1, "A#4 (70) IS a black key");
    CHECK(wb_score_is_black_key(64) == 0, "E4 (64) is NOT a black key");
    CHECK(wb_score_is_black_key(69) == 0, "A4 (69) is NOT a black key");

    /* ---- 6. Measure rendering produces non-empty output ---- */
    printf("\n== Measure rendering ==\n");

    {
        wb_note test_notes[4];
        memset(test_notes, 0, sizeof(test_notes));
        test_notes[0].pitch = 60; /* C4 */
        test_notes[0].vel = 100;
        test_notes[1].pitch = 64; /* E4 */
        test_notes[1].vel = 100;
        test_notes[2].pitch = 67; /* G4 */
        test_notes[2].vel = 100;
        test_notes[3].pitch = 72; /* C5 */
        test_notes[3].vel = 100;

        char render_buf[4096];
        int result = wb_score_render_measure(test_notes, 4, render_buf, sizeof(render_buf));
        CHECK(result > 0, "Measure rendering returns positive length");
        CHECK(strlen(render_buf) > 0, "Measure rendering produces non-empty output");
        CHECK(strchr(render_buf, 'o') != NULL, "Rendered output contains note heads ('o')");
        CHECK(strchr(render_buf, '-') != NULL, "Rendered output contains staff lines ('-')");

        printf("\n  -- Rendered output (C major arpeggio) --\n");
        /* Print with indentation */
        char *line = render_buf;
        while (*line) {
            char *nl = strchr(line, '\n');
            if (nl) {
                printf("  |%.*s|\n", (int)(nl - line), line);
                line = nl + 1;
            } else {
                printf("  |%s|\n", line);
                break;
            }
        }
    }

    /* Test with accidental notes */
    {
        wb_note acc_notes[3];
        memset(acc_notes, 0, sizeof(acc_notes));
        acc_notes[0].pitch = 61; /* C#4 */
        acc_notes[0].vel = 90;
        acc_notes[1].pitch = 66; /* F#4 */
        acc_notes[1].vel = 90;
        acc_notes[2].pitch = 68; /* G#4 */
        acc_notes[2].vel = 90;

        char render_buf[4096];
        int result = wb_score_render_measure(acc_notes, 3, render_buf, sizeof(render_buf));
        CHECK(result > 0, "Accidental measure rendering returns positive length");
        CHECK(strchr(render_buf, '#') != NULL, "Accidental measure contains sharp symbols ('#')");
    }

    /* ---- Summary ---- */
    printf("\n========================================\n");
    printf("Checks: %d | Failures: %d\n", checks, failures);
    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        printf("SOME TESTS FAILED\n");
        return 1;
    }
}