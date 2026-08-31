/* tests/test_midi_scale.c — headless test of the MIDI scale quantizer.
 * Verifies snapping, scale membership, boundary handling, and name lookup.
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
    /* 1. Create/destroy */
    wb_midi_scale *sc = wb_midi_scale_create();
    CHECK(sc != NULL, "wb_midi_scale_create() returns non-NULL");
    wb_midi_scale_destroy(sc);
    CHECK(1, "wb_midi_scale_destroy() does not crash");

    /* 2. Major scale snaps correctly (C major: root=0).
     * C major pitch classes: C(0), D(2), E(4), F(5), G(7), A(9), B(11) */
    sc = wb_midi_scale_create();
    wb_midi_scale_set_root(sc, 0);
    wb_midi_scale_set_type(sc, WB_SCALE_MAJOR);

    CHECK(wb_midi_scale_snap(sc, 60) == 60, "C4 snaps to C4 (in scale)");
    CHECK(wb_midi_scale_snap(sc, 61) == 62, "C#4 snaps to D4 (nearest up)");
    CHECK(wb_midi_scale_snap(sc, 62) == 62, "D4 snaps to D4 (in scale)");
    CHECK(wb_midi_scale_snap(sc, 63) == 64, "D#4 snaps to E4 (nearest up)");
    CHECK(wb_midi_scale_snap(sc, 64) == 64, "E4 snaps to E4 (in scale)");
    CHECK(wb_midi_scale_snap(sc, 65) == 65, "F4 snaps to F4 (in scale)");
    CHECK(wb_midi_scale_snap(sc, 66) == 67, "F#4 snaps to G4 (nearest up)");
    CHECK(wb_midi_scale_snap(sc, 67) == 67, "G4 snaps to G4 (in scale)");
    CHECK(wb_midi_scale_snap(sc, 68) == 69, "G#4 snaps to A4 (nearest up)");
    CHECK(wb_midi_scale_snap(sc, 69) == 69, "A4 snaps to A4 (in scale)");
    CHECK(wb_midi_scale_snap(sc, 70) == 71, "A#4 snaps to B4 (nearest up)");
    CHECK(wb_midi_scale_snap(sc, 71) == 71, "B4 snaps to B4 (in scale)");
    /* C5 should still be in scale */
    CHECK(wb_midi_scale_snap(sc, 72) == 72, "C5 snaps to C5 (in scale, next octave)");

    /* 3. Minor scale snaps correctly (A minor: root=9).
     * A minor pitch classes: A(9), B(11), C(0), D(2), E(4), F(5), G(7) */
    wb_midi_scale_set_root(sc, 9);
    wb_midi_scale_set_type(sc, WB_SCALE_MINOR);

    CHECK(wb_midi_scale_snap(sc, 69) == 69, "A4 snaps to A4 (in A minor)");
    CHECK(wb_midi_scale_snap(sc, 70) == 71, "A#4 snaps to B4 (in A minor)");
    CHECK(wb_midi_scale_snap(sc, 72) == 72, "C5 snaps to C5 (in A minor)");
    CHECK(wb_midi_scale_snap(sc, 73) == 74, "C#5 snaps to D5 (in A minor)");
    CHECK(wb_midi_scale_snap(sc, 74) == 74, "D5 snaps to D5 (in A minor)");
    CHECK(wb_midi_scale_snap(sc, 75) == 76, "D#5 snaps to E5 (in A minor)");
    CHECK(wb_midi_scale_snap(sc, 76) == 76, "E5 snaps to E5 (in A minor)");
    CHECK(wb_midi_scale_snap(sc, 77) == 77, "F5 snaps to F5 (in A minor)");
    CHECK(wb_midi_scale_snap(sc, 78) == 79, "F#5 snaps to G5 (in A minor)");

    /* 4. Pentatonic snaps correctly (C pentatonic major: root=0).
     * C pentatonic major pitch classes: C(0), D(2), E(4), G(7), A(9) */
    wb_midi_scale_set_root(sc, 0);
    wb_midi_scale_set_type(sc, WB_SCALE_PENTATONIC_MAJOR);

    CHECK(wb_midi_scale_snap(sc, 60) == 60, "C4 in C pentatonic major");
    CHECK(wb_midi_scale_snap(sc, 62) == 62, "D4 in C pentatonic major");
    CHECK(wb_midi_scale_snap(sc, 64) == 64, "E4 in C pentatonic major");
    CHECK(wb_midi_scale_snap(sc, 65) == 64, "F4 snaps down to E4 in pentatonic");
    CHECK(wb_midi_scale_snap(sc, 67) == 67, "G4 in C pentatonic major");
    CHECK(wb_midi_scale_snap(sc, 69) == 69, "A4 in C pentatonic major");
    CHECK(wb_midi_scale_is_in_scale(sc, 71) == 0, "B4 is NOT in C pentatonic major");
    CHECK(wb_midi_scale_snap(sc, 71) == 72, "B4 snaps to C5 in C pentatonic (nearest, 1 up vs 2 down)");

    /* 5. Chromatic passes all through unchanged */
    wb_midi_scale_set_type(sc, WB_SCALE_CHROMATIC);
    int chrom_ok = 1;
    for (int n = 0; n < 128; n++) {
        if (wb_midi_scale_snap(sc, n) != n) {
            chrom_ok = 0;
            break;
        }
    }
    CHECK(chrom_ok, "Chromatic passes all 128 notes through unchanged");

    /* 6. snap_up and snap_down work (C major) */
    wb_midi_scale_set_root(sc, 0);
    wb_midi_scale_set_type(sc, WB_SCALE_MAJOR);

    CHECK(wb_midi_scale_snap_up(sc, 61) == 62, "snap_up(C#4) = D4 in C major");
    CHECK(wb_midi_scale_snap_down(sc, 61) == 60, "snap_down(C#4) = C4 in C major");
    CHECK(wb_midi_scale_snap_up(sc, 66) == 67, "snap_up(F#4) = G4 in C major");
    CHECK(wb_midi_scale_snap_down(sc, 66) == 65, "snap_down(F#4) = F4 in C major");
    /* In-scale notes stay put */
    CHECK(wb_midi_scale_snap_up(sc, 60) == 60, "snap_up(C4) = C4 (in scale)");
    CHECK(wb_midi_scale_snap_down(sc, 60) == 60, "snap_down(C4) = C4 (in scale)");

    /* 7. is_in_scale returns correct bools (C major) */
    CHECK(wb_midi_scale_is_in_scale(sc, 60) == 1, "C4 is in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 62) == 1, "D4 is in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 64) == 1, "E4 is in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 65) == 1, "F4 is in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 67) == 1, "G4 is in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 69) == 1, "A4 is in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 71) == 1, "B4 is in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 61) == 0, "C#4 is NOT in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 63) == 0, "D#4 is NOT in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 66) == 0, "F#4 is NOT in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 68) == 0, "G#4 is NOT in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 70) == 0, "A#4 is NOT in C major");

    /* 8. Boundary notes (0 and 127) */
    wb_midi_scale_set_root(sc, 0);
    wb_midi_scale_set_type(sc, WB_SCALE_MAJOR);
    /* MIDI note 0 = C, which is in C major */
    CHECK(wb_midi_scale_snap(sc, 0) == 0, "Note 0 (C-1) snaps correctly in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 0) == 1, "Note 0 is in C major");
    /* MIDI note 127 = G, which is in C major */
    CHECK(wb_midi_scale_snap(sc, 127) == 127, "Note 127 (G9) snaps correctly in C major");
    CHECK(wb_midi_scale_is_in_scale(sc, 127) == 1, "Note 127 is in C major (G)");
    /* Test snap_up/down at boundaries */
    CHECK(wb_midi_scale_snap_up(sc, 0) == 0, "snap_up(0) = 0 (in scale)");
    CHECK(wb_midi_scale_snap_down(sc, 0) == 0, "snap_down(0) = 0 (in scale)");
    CHECK(wb_midi_scale_snap_up(sc, 127) == 127, "snap_up(127) = 127 (in scale)");
    CHECK(wb_midi_scale_snap_down(sc, 127) == 127, "snap_down(127) = 127 (in scale)");

    /* 9. Get name returns non-NULL */
    CHECK(wb_midi_scale_get_name(WB_SCALE_MAJOR) != NULL, "get_name(MAJOR) non-NULL");
    CHECK(wb_midi_scale_get_name(WB_SCALE_MINOR) != NULL, "get_name(MINOR) non-NULL");
    CHECK(wb_midi_scale_get_name(WB_SCALE_CHROMATIC) != NULL, "get_name(CHROMATIC) non-NULL");
    CHECK(wb_midi_scale_get_name(WB_SCALE_PENTATONIC_MAJOR) != NULL, "get_name(PENTATONIC_MAJOR) non-NULL");
    CHECK(wb_midi_scale_get_name(WB_SCALE_BLUES) != NULL, "get_name(BLUES) non-NULL");
    CHECK(wb_midi_scale_get_name(-1) == NULL, "get_name(-1) returns NULL");
    CHECK(wb_midi_scale_get_name(100) == NULL, "get_name(100) returns NULL (out of range)");
    /* Verify a name's content */
    CHECK(strcmp(wb_midi_scale_get_name(WB_SCALE_MAJOR), "Major") == 0, "get_name(MAJOR) == \"Major\"");
    CHECK(strcmp(wb_midi_scale_get_name(WB_SCALE_HARMONIC_MINOR), "Harmonic Minor") == 0,
          "get_name(HARMONIC_MINOR) == \"Harmonic Minor\"");

    /* 10. Additional: different root notes */
    wb_midi_scale_set_root(sc, 7);  /* G */
    wb_midi_scale_set_type(sc, WB_SCALE_MAJOR);
    /* G major: G(7), A(9), B(11), C(0), D(2), E(4), F#(6) */
    CHECK(wb_midi_scale_is_in_scale(sc, 67) == 1, "G4 is in G major");
    CHECK(wb_midi_scale_is_in_scale(sc, 71) == 1, "B4 is in G major");
    CHECK(wb_midi_scale_is_in_scale(sc, 73) == 0, "C#5 is NOT in G major");
    CHECK(wb_midi_scale_is_in_scale(sc, 70) == 0, "A#4 is NOT in G major");
    CHECK(wb_midi_scale_snap(sc, 70) == 71, "A#4 snaps to B4 in G major");

    wb_midi_scale_destroy(sc);

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}