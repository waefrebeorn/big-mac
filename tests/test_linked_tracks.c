/* tests/test_linked_tracks.c — linked track group editing tests.
 * Verifies Ableton-style simultaneous multi-track editing: create/remove
 * groups, add/remove tracks, and linked move/trim/delete/add-note operations.
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

/* Helper: add a MIDI clip to a track */
static void add_midi_clip(wb_track *tr, double start, double len) {
    tr->clips = realloc(tr->clips, (tr->clip_count + 1) * sizeof(wb_clip));
    wb_clip *cl = &tr->clips[tr->clip_count++];
    memset(cl, 0, sizeof(*cl));
    cl->type = 0;
    cl->start = start;
    cl->length = len;
}

int main(void) {
    printf("=== test_linked_tracks ===\n");

    /* 1. Create link group with 3 tracks */
    {
        printf("\n-- Test 1: Create link group with 3 tracks --\n");
        wb_session *s = wb_session_create();
        CHECK(s != NULL, "session created");

        int t0 = (int)s->track_count;
        wb_session_add_track(s, "Track A", WB_TRACK_KIND_INSTR);
        int t1 = (int)s->track_count;
        wb_session_add_track(s, "Track B", WB_TRACK_KIND_INSTR);
        int t2 = (int)s->track_count;
        wb_session_add_track(s, "Track C", WB_TRACK_KIND_INSTR);

        int indices[] = {t0, t1, t2};
        int gid = wb_session_create_link_group(s, indices, 3);
        CHECK(gid >= 0, "link group created");
        CHECK(wb_session_link_group_count(s) == 1, "group count == 1");

        /* Verify group contents */
        int out[8];
        int n = wb_session_get_link_group(s, gid, out, 8);
        CHECK(n == 3, "get_link_group returns 3 tracks");
        CHECK(out[0] == t0 && out[1] == t1 && out[2] == t2, "group has correct tracks");

        wb_session_destroy(s);
    }

    /* 2. Remove link group */
    {
        printf("\n-- Test 2: Remove link group --\n");
        wb_session *s = wb_session_create();
        wb_session_add_track(s, "T0", WB_TRACK_KIND_INSTR);
        wb_session_add_track(s, "T1", WB_TRACK_KIND_INSTR);
        int idx[] = {0, 1};
        int gid = wb_session_create_link_group(s, idx, 2);
        CHECK(gid >= 0, "group created");
        CHECK(wb_session_remove_link_group(s, gid) == 0, "group removed");
        CHECK(wb_session_link_group_count(s) == 0, "group count == 0 after removal");

        wb_session_destroy(s);
    }

    /* 3. Add/remove track from group */
    {
        printf("\n-- Test 3: Add/remove track from group --\n");
        wb_session *s = wb_session_create();
        wb_session_add_track(s, "T0", WB_TRACK_KIND_INSTR);
        wb_session_add_track(s, "T1", WB_TRACK_KIND_INSTR);
        wb_session_add_track(s, "T2", WB_TRACK_KIND_INSTR);
        wb_session_add_track(s, "T3", WB_TRACK_KIND_INSTR);

        int idx[] = {0, 1};
        int gid = wb_session_create_link_group(s, idx, 2);
        CHECK(gid >= 0, "group created with 2 tracks");

        CHECK(wb_session_add_to_link_group(s, gid, 2) == 0, "added track 2 to group");
        CHECK(wb_session_add_to_link_group(s, gid, 3) == 0, "added track 3 to group");

        int out[8];
        int n = wb_session_get_link_group(s, gid, out, 8);
        CHECK(n == 4, "group now has 4 tracks");

        CHECK(wb_session_remove_from_link_group(s, gid, 2) == 0, "removed track 2");
        n = wb_session_get_link_group(s, gid, out, 8);
        CHECK(n == 3, "group has 3 tracks after removal");
        CHECK(out[0] == 0 && out[1] == 1 && out[2] == 3, "remaining tracks correct");

        wb_session_destroy(s);
    }

    /* 4. Linked move clip affects all tracks */
    {
        printf("\n-- Test 4: Linked move clip affects all tracks --\n");
        wb_session *s = wb_session_create();
        int t0 = (int)s->track_count;
        wb_session_add_track(s, "A", WB_TRACK_KIND_INSTR);
        int t1 = (int)s->track_count;
        wb_session_add_track(s, "B", WB_TRACK_KIND_INSTR);
        int t2 = (int)s->track_count;
        wb_session_add_track(s, "C", WB_TRACK_KIND_INSTR);

        /* Add a clip at index 0 to each track */
        add_midi_clip(&s->tracks[t0], 100.0, 500.0);
        add_midi_clip(&s->tracks[t1], 200.0, 500.0);
        add_midi_clip(&s->tracks[t2], 300.0, 500.0);

        int idx[] = {t0, t1, t2};
        int gid = wb_session_create_link_group(s, idx, 3);
        CHECK(gid >= 0, "group created");

        CHECK(wb_session_linked_move_clip(s, gid, t0, 0, 1000.0) == 0, "linked move succeeded");
        CHECK(s->tracks[t0].clips[0].start == 1000.0, "track A clip moved to 1000");
        CHECK(s->tracks[t1].clips[0].start == 1000.0, "track B clip moved to 1000");
        CHECK(s->tracks[t2].clips[0].start == 1000.0, "track C clip moved to 1000");

        wb_session_destroy(s);
    }

    /* 5. Linked trim affects all tracks */
    {
        printf("\n-- Test 5: Linked trim affects all tracks --\n");
        wb_session *s = wb_session_create();
        int t0 = (int)s->track_count;
        wb_session_add_track(s, "A", WB_TRACK_KIND_AUDIO);
        int t1 = (int)s->track_count;
        wb_session_add_track(s, "B", WB_TRACK_KIND_AUDIO);

        add_midi_clip(&s->tracks[t0], 0.0, 1000.0);
        add_midi_clip(&s->tracks[t1], 0.0, 1000.0);

        int idx[] = {t0, t1};
        int gid = wb_session_create_link_group(s, idx, 2);

        /* Trim head by 100, tail by 50 */
        CHECK(wb_session_linked_trim_clip(s, gid, t0, 0, 100.0, 50.0) == 0, "trim succeeded");
        CHECK(s->tracks[t0].clips[0].start == 100.0, "track A start == 100");
        CHECK(s->tracks[t0].clips[0].length == 850.0, "track A length == 850");
        CHECK(s->tracks[t1].clips[0].start == 100.0, "track B start == 100");
        CHECK(s->tracks[t1].clips[0].length == 850.0, "track B length == 850");

        wb_session_destroy(s);
    }

    /* 6. Linked delete affects all tracks */
    {
        printf("\n-- Test 6: Linked delete affects all tracks --\n");
        wb_session *s = wb_session_create();
        int t0 = (int)s->track_count;
        wb_session_add_track(s, "A", WB_TRACK_KIND_INSTR);
        int t1 = (int)s->track_count;
        wb_session_add_track(s, "B", WB_TRACK_KIND_INSTR);

        /* Add 2 clips to each track */
        add_midi_clip(&s->tracks[t0], 0.0, 500.0);
        add_midi_clip(&s->tracks[t0], 600.0, 500.0);
        add_midi_clip(&s->tracks[t1], 0.0, 500.0);
        add_midi_clip(&s->tracks[t1], 600.0, 500.0);

        int idx[] = {t0, t1};
        int gid = wb_session_create_link_group(s, idx, 2);

        CHECK(s->tracks[t0].clip_count == 2, "track A has 2 clips before delete");
        CHECK(s->tracks[t1].clip_count == 2, "track B has 2 clips before delete");

        CHECK(wb_session_linked_delete_clip(s, gid, t0, 0) == 0, "delete succeeded");
        CHECK(s->tracks[t0].clip_count == 1, "track A has 1 clip after delete");
        CHECK(s->tracks[t1].clip_count == 1, "track B has 1 clip after delete");

        wb_session_destroy(s);
    }

    /* 7. Linked add note affects all tracks */
    {
        printf("\n-- Test 7: Linked add note affects all tracks --\n");
        wb_session *s = wb_session_create();
        int t0 = (int)s->track_count;
        wb_session_add_track(s, "A", WB_TRACK_KIND_INSTR);
        int t1 = (int)s->track_count;
        wb_session_add_track(s, "B", WB_TRACK_KIND_INSTR);
        int t2 = (int)s->track_count;
        wb_session_add_track(s, "C", WB_TRACK_KIND_INSTR);

        int idx[] = {t0, t1, t2};
        int gid = wb_session_create_link_group(s, idx, 3);

        CHECK(wb_session_linked_add_note(s, gid, t0, 0.0, 44100.0, 60, 100) == 0, "add note succeeded");
        CHECK(s->tracks[t0].clip_count == 1, "track A has 1 clip");
        CHECK(s->tracks[t0].clips[0].note_count == 1, "track A has 1 note");
        CHECK(s->tracks[t1].clips[0].note_count == 1, "track B has 1 note");
        CHECK(s->tracks[t2].clips[0].note_count == 1, "track C has 1 note");

        CHECK(s->tracks[t0].clips[0].notes[0].pitch == 60, "note pitch == 60");
        CHECK(s->tracks[t1].clips[0].notes[0].vel == 100, "note vel == 100");

        /* Add another note */
        CHECK(wb_session_linked_add_note(s, gid, t0, 44100.0, 22050.0, 64, 90) == 0, "add note 2 succeeded");
        CHECK(s->tracks[t0].clips[0].note_count == 2, "track A has 2 notes");
        CHECK(s->tracks[t1].clips[0].note_count == 2, "track B has 2 notes");
        CHECK(s->tracks[t2].clips[0].note_count == 2, "track C has 2 notes");

        wb_session_destroy(s);
    }

    /* 8. Get link group returns correct tracks */
    {
        printf("\n-- Test 8: Get link group returns correct tracks --\n");
        wb_session *s = wb_session_create();
        wb_session_add_track(s, "T0", WB_TRACK_KIND_INSTR);
        wb_session_add_track(s, "T1", WB_TRACK_KIND_INSTR);
        wb_session_add_track(s, "T2", WB_TRACK_KIND_INSTR);
        wb_session_add_track(s, "T3", WB_TRACK_KIND_INSTR);

        int idx[] = {0, 2, 3};
        int gid = wb_session_create_link_group(s, idx, 3);
        CHECK(gid >= 0, "group created");

        int out[4];
        int n = wb_session_get_link_group(s, gid, out, 4);
        CHECK(n == 3, "returned 3 tracks");
        CHECK(out[0] == 0 && out[1] == 2 && out[2] == 3, "tracks match input");

        /* Test limited buffer */
        int small[2];
        n = wb_session_get_link_group(s, gid, small, 2);
        CHECK(n == 2, "limited buffer returns 2");

        wb_session_destroy(s);
    }

    /* 9. Invalid group_id returns error */
    {
        printf("\n-- Test 9: Invalid group_id returns error --\n");
        wb_session *s = wb_session_create();
        wb_session_add_track(s, "T0", WB_TRACK_KIND_INSTR);

        CHECK(wb_session_remove_link_group(s, 0) == -1, "remove nonexistent group");
        CHECK(wb_session_remove_link_group(s, -1) == -1, "remove negative group_id");
        CHECK(wb_session_add_to_link_group(s, 0, 0) == -1, "add to nonexistent group");
        CHECK(wb_session_remove_from_link_group(s, 0, 0) == -1, "remove from nonexistent group");
        CHECK(wb_session_linked_move_clip(s, 0, 0, 0, 100.0) == -1, "move with bad group");
        CHECK(wb_session_linked_trim_clip(s, 0, 0, 0, 10.0, 10.0) == -1, "trim with bad group");
        CHECK(wb_session_linked_delete_clip(s, 0, 0, 0) == -1, "delete with bad group");
        CHECK(wb_session_linked_add_note(s, 0, 0, 0.0, 100.0, 60, 100) == -1, "add note with bad group");

        int out[4];
        CHECK(wb_session_get_link_group(s, 0, out, 4) == -1, "get with bad group_id");
        CHECK(wb_session_get_link_group(s, -1, out, 4) == -1, "get with negative group_id");

        wb_session_destroy(s);
    }

    printf("\n=== Results: %d/%d passed, %d FAILED ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}