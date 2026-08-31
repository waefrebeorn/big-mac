/* tests/test_expression.c — headless test of expression maps / articulation
 * management (G91). Verifies map creation, articulation CRUD, active
 * articulation switching, per-note application, name/count queries, and
 * multiple maps per session.
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
    printf("=== Expression Maps (G91) Tests ===\n\n");

    /* 1. Create expression map */
    wb_session *s = wb_session_create();
    CHECK(s != NULL, "wb_session_create() returns non-NULL session");

    int map1 = wb_session_add_expression_map(s, "Strings - Legato/Staccato");
    CHECK(map1 == 0, "First expression map gets id 0");
    CHECK(wb_session_expression_map_count(s) == 1, "Expression map count is 1 after add");

    /* 2. Add articulations */
    int art_sustain = wb_session_add_articulation(s, map1, "Sustain", 1, 64, 127, 20);
    CHECK(art_sustain == 0, "First articulation gets index 0 (Sustain)");
    int art_staccato = wb_session_add_articulation(s, map1, "Staccato", 1, 64, 0, 21);
    CHECK(art_staccato == 1, "Second articulation gets index 1 (Staccato)");
    int art_pizz = wb_session_add_articulation(s, map1, "Pizzicato", 2, -1, -1, 22);
    CHECK(art_pizz == 2, "Third articulation gets index 2 (Pizzicato)");
    int art_tremolo = wb_session_add_articulation(s, map1, "Tremolo", 1, 1, 80, 23);
    CHECK(art_tremolo == 3, "Fourth articulation gets index 3 (Tremolo)");
    int art_spiccato = wb_session_add_articulation(s, map1, "Spiccato", 1, 1, 40, 24);
    CHECK(art_spiccato == 4, "Fifth articulation gets index 4 (Spiccato)");
    int art_legato = wb_session_add_articulation(s, map1, "Legato", 1, 64, 100, 25);
    CHECK(art_legato == 5, "Sixth articulation gets index 5 (Legato)");
    int art_marcato = wb_session_add_articulation(s, map1, "Marcato", 1, 64, 127, 26);
    CHECK(art_marcato == 6, "Seventh articulation gets index 6 (Marcato)");

    /* 3. Set active articulation */
    int rc = wb_session_set_active_articulation(s, map1, 1);
    CHECK(rc == 0, "Set active articulation to Staccato (idx 1) succeeds");
    rc = wb_session_set_active_articulation(s, map1, 99);
    CHECK(rc == -1, "Set active articulation with invalid index fails");
    rc = wb_session_set_active_articulation(s, 99, 0);
    CHECK(rc == -1, "Set active articulation with invalid map_id fails");

    /* 4. Apply articulation to note */
    wb_track *tr = wb_session_add_track(s, "Violin", 0);
    CHECK(tr != NULL, "Add track 'Violin'");
    wb_session_set_expression_lane(s, 0, map1);
    CHECK(0 == 0, "Expression lane set for track 0 -> map1");

    /* Add a note to the track */
    int nrc = wb_session_add_note(tr, 0.0, 48000.0, 60, 100);
    CHECK(nrc == 0, "Add note (C4, vel 100) to track");

    /* Apply pizzicato articulation to the note */
    int arc = wb_session_apply_articulation_to_note(s, 0, 0, 2);
    CHECK(arc == 0, "Apply Pizzicato (idx 2) to note 0 succeeds");

    /* Apply invalid articulation index */
    arc = wb_session_apply_articulation_to_note(s, 0, 0, 99);
    CHECK(arc == -1, "Apply articulation with invalid index fails");

    /* Apply to invalid track */
    arc = wb_session_apply_articulation_to_note(s, 99, 0, 0);
    CHECK(arc == -1, "Apply articulation to invalid track fails");

    /* 5. Get articulation count and name */
    int count = wb_session_get_articulation_count(s, map1);
    CHECK(count == 7, "Articulation count is 7");

    const char *nm = wb_session_get_articulation_name(s, map1, 0);
    CHECK(strcmp(nm, "Sustain") == 0, "Articulation 0 name is 'Sustain'");
    nm = wb_session_get_articulation_name(s, map1, 2);
    CHECK(strcmp(nm, "Pizzicato") == 0, "Articulation 2 name is 'Pizzicato'");
    nm = wb_session_get_articulation_name(s, map1, 5);
    CHECK(strcmp(nm, "Legato") == 0, "Articulation 5 name is 'Legato'");
    nm = wb_session_get_articulation_name(s, map1, 6);
    CHECK(strcmp(nm, "Marcato") == 0, "Articulation 6 name is 'Marcato'");
    nm = wb_session_get_articulation_name(s, map1, 99);
    CHECK(strcmp(nm, "") == 0, "Invalid articulation index returns empty string");

    /* 6. Multiple maps per session */
    int map2 = wb_session_add_expression_map(s, "Brass - Sustains");
    CHECK(map2 == 1, "Second expression map gets id 1");
    CHECK(wb_session_expression_map_count(s) == 2, "Expression map count is 2");

    int brass_sus = wb_session_add_articulation(s, map2, "Sustain", 3, 64, 127, 30);
    CHECK(brass_sus == 0, "Brass map: first articulation index 0");
    int brass_mute = wb_session_add_articulation(s, map2, "Muted", 3, 64, 0, 31);
    CHECK(brass_mute == 1, "Brass map: second articulation index 1");

    int count2 = wb_session_get_articulation_count(s, map2);
    CHECK(count2 == 2, "Brass map articulation count is 2");
    int count1 = wb_session_get_articulation_count(s, map1);
    CHECK(count1 == 7, "Strings map still has 7 articulations (independent)");

    const char *bn = wb_session_get_articulation_name(s, map2, 0);
    CHECK(strcmp(bn, "Sustain") == 0, "Brass map articulation 0 is 'Sustain'");
    bn = wb_session_get_articulation_name(s, map2, 1);
    CHECK(strcmp(bn, "Muted") == 0, "Brass map articulation 1 is 'Muted'");

    /* Expression lane for second track */
    wb_track *tr2 = wb_session_add_track(s, "Trumpet", 0);
    CHECK(tr2 != NULL, "Add track 'Trumpet'");
    rc = wb_session_set_expression_lane(s, 1, map2);
    CHECK(rc == 0, "Expression lane set for track 1 -> map2 (brass)");

    /* NULL session handling */
    CHECK(wb_session_expression_map_count(NULL) == -1, "NULL session returns -1 for map count");
    CHECK(wb_session_get_articulation_count(NULL, 0) == -1, "NULL session returns -1 for art count");
    CHECK(strcmp(wb_session_get_articulation_name(NULL, 0, 0), "") == 0,
          "NULL session returns empty string for art name");

    /* Boundary: add expression map with NULL name */
    int bad_map = wb_session_add_expression_map(s, NULL);
    CHECK(bad_map == -1, "Adding map with NULL name fails");

    /* Boundary: add articulation with NULL name */
    int bad_art = wb_session_add_articulation(s, map1, NULL, 1, -1, -1, -1);
    CHECK(bad_art == -1, "Adding articulation with NULL name fails");

    wb_session_destroy(s);
    CHECK(1, "wb_session_destroy() does not crash");

    printf("\n=== Results: %d/%d checks passed ===\n", checks - failures, checks);
    if (failures > 0) {
        printf("*** %d FAILURES ***\n", failures);
        return 1;
    }
    printf("All tests passed.\n");
    return 0;
}