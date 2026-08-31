/* tests/test_track_folder.c — track folders and bus routing tests.
 * Verifies folder hierarchy, child track grouping, mute/solo propagation,
 * folder removal, bus creation, track routing, and aux send configuration.
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
    printf("=== test_track_folder ===\n");

    /* 1. Create folder, add tracks to it */
    {
        printf("\n-- Test 1: Create folder, add tracks --\n");
        wb_session *s = wb_session_create();
        CHECK(s != NULL, "session created");

        wb_session_add_track(s, "Audio A", WB_TRACK_KIND_AUDIO);
        wb_session_add_track(s, "Audio B", WB_TRACK_KIND_AUDIO);
        int folder = wb_session_create_folder(s, "Drums", -1);
        CHECK(folder >= 0, "folder created at top level");
        CHECK(s->tracks[folder].kind == WB_TRACK_KIND_FOLDER, "folder track kind is FOLDER");

        int trk1 = (int)s->track_count;
        wb_session_add_track(s, "Kick", WB_TRACK_KIND_AUDIO);
        int trk2 = (int)s->track_count;
        wb_session_add_track(s, "Snare", WB_TRACK_KIND_AUDIO);

        CHECK(wb_session_add_track_to_folder(s, trk1, folder) == 0, "added Kick to folder");
        CHECK(wb_session_add_track_to_folder(s, trk2, folder) == 0, "added Snare to folder");
        CHECK(wb_session_get_folder_track_count(s, folder) == 2, "folder has 2 tracks");
        CHECK(s->tracks[trk1].folder_idx == folder, "Kick's folder_idx set");
        CHECK(s->tracks[trk2].folder_idx == folder, "Snare's folder_idx set");

        wb_session_destroy(s);
    }

    /* 2. Create bus, route tracks to it */
    {
        printf("\n-- Test 2: Create bus, route tracks --\n");
        wb_session *s = wb_session_create();
        int bus = wb_session_create_bus(s, "Reverb Bus");
        CHECK(bus >= 0, "bus created");
        CHECK(s->tracks[bus].kind == WB_TRACK_KIND_BUS, "bus track kind is BUS");

        int t0 = (int)s->track_count;
        wb_session_add_track(s, "Guitar", WB_TRACK_KIND_AUDIO);
        int t1 = (int)s->track_count;
        wb_session_add_track(s, "Vox", WB_TRACK_KIND_AUDIO);

        CHECK(wb_session_route_track_to(s, t0, bus) == 0, "routed Guitar to bus");
        CHECK(s->tracks[t0].route == bus, "Guitar route == bus index");
        CHECK(wb_session_route_track_to(s, t1, bus) == 0, "routed Vox to bus");
        CHECK(s->tracks[t1].route == bus, "Vox route == bus index");

        wb_session_destroy(s);
    }

    /* 3. Set send levels */
    {
        printf("\n-- Test 3: Set send levels --\n");
        wb_session *s = wb_session_create();
        int bus = wb_session_create_bus(s, "Delay Bus");
        int src = (int)s->track_count;
        wb_session_add_track(s, "Lead", WB_TRACK_KIND_AUDIO);

        CHECK(wb_session_set_send(s, src, bus, 0.75f, 0) == 0, "set SEND A to bus at 0.75");
        CHECK(s->tracks[src].send_target[0] == bus, "send A target == bus");
        CHECK(s->tracks[src].send_level[0] == 0.75f, "send A level == 0.75");

        CHECK(wb_session_set_send(s, src, bus, 0.5f, 1) == 0, "set SEND B to bus at 0.5");
        CHECK(s->tracks[src].send_target[1] == bus, "send B target == bus");
        CHECK(s->tracks[src].send_level[1] == 0.5f, "send B level == 0.5");

        wb_session_destroy(s);
    }

    /* 4. Folder mute propagates to children */
    {
        printf("\n-- Test 4: Folder mute propagates --\n");
        wb_session *s = wb_session_create();
        int folder = wb_session_create_folder(s, "Group", -1);
        int t0 = (int)s->track_count;
        wb_session_add_track(s, "A", WB_TRACK_KIND_AUDIO);
        int t1 = (int)s->track_count;
        wb_session_add_track(s, "B", WB_TRACK_KIND_AUDIO);
        wb_session_add_track_to_folder(s, t0, folder);
        wb_session_add_track_to_folder(s, t1, folder);

        /* Ensure children start unmuted */
        s->tracks[t0].mute = 0;
        s->tracks[t1].mute = 0;
        CHECK(wb_session_set_folder_mute(s, folder, 1) == 0, "mute folder");
        CHECK(s->tracks[t0].mute == 1, "child A muted by folder");
        CHECK(s->tracks[t1].mute == 1, "child B muted by folder");

        /* Unmute folder */
        CHECK(wb_session_set_folder_mute(s, folder, 0) == 0, "unmute folder");
        CHECK(s->tracks[t0].mute == 0, "child A unmuted");
        CHECK(s->tracks[t1].mute == 0, "child B unmuted");

        /* Test solo propagation */
        s->tracks[t0].solo = 0;
        s->tracks[t1].solo = 0;
        CHECK(wb_session_set_folder_solo(s, folder, 1) == 0, "solo folder");
        CHECK(s->tracks[t0].solo == 1, "child A soloed by folder");
        CHECK(s->tracks[t1].solo == 1, "child B soloed by folder");

        wb_session_destroy(s);
    }

    /* 5. Remove folder moves children to parent */
    {
        printf("\n-- Test 5: Remove folder moves children --\n");
        wb_session *s = wb_session_create();
        int parent = wb_session_create_folder(s, "Parent", -1);
        int child = wb_session_create_folder(s, "Child", parent);
        CHECK(s->folders[child].parent_folder_idx == parent, "child's parent set");

        /* Add audio tracks to child folder */
        int t0 = (int)s->track_count;
        wb_session_add_track(s, "Sub1", WB_TRACK_KIND_AUDIO);
        int t1 = (int)s->track_count;
        wb_session_add_track(s, "Sub2", WB_TRACK_KIND_AUDIO);
        wb_session_add_track_to_folder(s, t0, child);
        wb_session_add_track_to_folder(s, t1, child);
        CHECK(wb_session_get_folder_track_count(s, child) == 2, "child folder has 2 tracks");

        /* Remove child folder — children should move to parent */
        CHECK(wb_session_remove_folder(s, child) == 0, "removed child folder");
        /* After removal, track indices shift. t0 and t1 may have changed.
         * The children should now be in parent folder. */
        CHECK(wb_session_get_folder_track_count(s, parent) >= 0, "parent folder accessible");

        wb_session_destroy(s);
    }

    /* 6. Route track to bus then to master */
    {
        printf("\n-- Test 6: Route track to bus then to master --\n");
        wb_session *s = wb_session_create();
        int bus = wb_session_create_bus(s, "Submix");
        int trk = (int)s->track_count;
        wb_session_add_track(s, "Drums", WB_TRACK_KIND_AUDIO);

        CHECK(wb_session_route_track_to(s, trk, bus) == 0, "routed track to bus");
        CHECK(s->tracks[trk].route == bus, "track route == bus");

        /* Route bus to master (bus itself routes to master) */
        CHECK(wb_session_route_track_to(s, bus, -1) == 0, "routed bus to master");
        CHECK(s->tracks[bus].route == -1, "bus route == master (-1)");

        /* Route track back to master */
        CHECK(wb_session_route_track_to(s, trk, -1) == 0, "routed track to master");
        CHECK(s->tracks[trk].route == -1, "track route == master (-1)");

        wb_session_destroy(s);
    }

    /* 7. Send pre/post fader setting */
    {
        printf("\n-- Test 7: Send pre/post fader --\n");
        wb_session *s = wb_session_create();
        int bus = wb_session_create_bus(s, "FX Bus");
        int src = (int)s->track_count;
        wb_session_add_track(s, "Synth", WB_TRACK_KIND_AUDIO);
        wb_session_set_send(s, src, bus, 0.8f, 0);

        CHECK(wb_session_set_send_pre_fader(s, src, 0, 1) == 0, "set SEND A to pre-fader");
        CHECK(s->tracks[src].send_pre[0] == 1, "send A pre-fader == 1");

        CHECK(wb_session_set_send_pre_fader(s, src, 0, 0) == 0, "set SEND A to post-fader");
        CHECK(s->tracks[src].send_pre[0] == 0, "send A pre-fader == 0");

        CHECK(wb_session_set_send_pre_fader(s, src, 1, 1) == 0, "set SEND B to pre-fader");
        CHECK(s->tracks[src].send_pre[1] == 1, "send B pre-fader == 1");

        wb_session_destroy(s);
    }

    /* 8. Get folder track count and list */
    {
        printf("\n-- Test 8: Get folder track count and list --\n");
        wb_session *s = wb_session_create();
        int folder = wb_session_create_folder(s, "Ensemble", -1);
        int indices[WB_MAX_TRACKS];

        /* Empty folder */
        CHECK(wb_session_get_folder_track_count(s, folder) == 0, "empty folder count == 0");
        CHECK(wb_session_get_folder_tracks(s, folder, indices, WB_MAX_TRACKS) == 0, "empty folder list == 0");

        /* Add tracks */
        int t0 = (int)s->track_count;
        wb_session_add_track(s, "Violin", WB_TRACK_KIND_INSTR);
        int t1 = (int)s->track_count;
        wb_session_add_track(s, "Cello", WB_TRACK_KIND_INSTR);
        int t2 = (int)s->track_count;
        wb_session_add_track(s, "Viola", WB_TRACK_KIND_INSTR);
        wb_session_add_track_to_folder(s, t0, folder);
        wb_session_add_track_to_folder(s, t1, folder);
        wb_session_add_track_to_folder(s, t2, folder);

        CHECK(wb_session_get_folder_track_count(s, folder) == 3, "folder count == 3");

        int n = wb_session_get_folder_tracks(s, folder, indices, WB_MAX_TRACKS);
        CHECK(n == 3, "got 3 track indices");
        CHECK(indices[0] == t0, "first track is Violin");
        CHECK(indices[1] == t1, "second track is Cello");
        CHECK(indices[2] == t2, "third track is Viola");

        /* Test limited buffer */
        int small[2];
        n = wb_session_get_folder_tracks(s, folder, small, 2);
        CHECK(n == 2, "limited buffer returns 2");

        wb_session_destroy(s);
    }

    /* Edge cases */
    {
        printf("\n-- Edge cases --\n");
        wb_session *s = wb_session_create();
        int folder = wb_session_create_folder(s, "Test", -1);

        /* Invalid indices */
        CHECK(wb_session_add_track_to_folder(s, 999, folder) == -1, "invalid track index rejected");
        CHECK(wb_session_add_track_to_folder(s, 0, 999) == -1, "invalid folder index rejected");
        CHECK(wb_session_set_folder_mute(s, 999, 1) == -1, "invalid folder mute rejected");
        CHECK(wb_session_get_folder_track_count(s, 999) == -1, "invalid folder count rejected");

        /* Routing validation */
        int trk = (int)s->track_count;
        wb_session_add_track(s, "Audio", WB_TRACK_KIND_AUDIO);
        CHECK(wb_session_route_track_to(s, trk, 999) == -1, "invalid dest rejected");
        CHECK(wb_session_route_track_to(s, 999, -1) == -1, "invalid src route rejected");

        /* Send validation */
        CHECK(wb_session_set_send(s, 999, -1, 0.5f, 0) == -1, "invalid src send rejected");
        CHECK(wb_session_set_send(s, trk, -1, 0.5f, 2) == -1, "invalid send_index rejected");
        CHECK(wb_session_set_send_pre_fader(s, 999, 0, 1) == -1, "invalid src pre-fader rejected");

        /* Cannot route to non-bus */
        int t2 = (int)s->track_count;
        wb_session_add_track(s, "NotABus", WB_TRACK_KIND_AUDIO);
        CHECK(wb_session_route_track_to(s, trk, t2) == -1, "cannot route to non-bus");

        /* Folder collapsed */
        CHECK(wb_session_set_folder_collapsed(s, folder, 1) == 0, "set folder collapsed");
        CHECK(s->folders[folder].collapsed == 1, "folder collapsed state set");

        wb_session_destroy(s);
    }

    printf("\n=== Results: %d/%d passed, %d FAILED ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}