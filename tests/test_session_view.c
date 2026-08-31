/* test_session_view.c — gate test for wb_session_view (Ableton-style clip launcher).
 * Verifies: create slot, launch clip, stop clip, launch scene, stop all,
 * launch mode behavior, quantize setting, playing clip tracking. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "wbus.h"

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

int main(void) {
    /* Session pointer is context-only for session_view; we can pass NULL
     * or a real session. For the wb_session* convenience API test, we
     * need a session, so we create a minimal one. */
    wb_session *session = NULL; /* session_view doesn't dereference it */

    /* ---- Test 1: Create slot ---- */
    TEST("Create slot");
    {
        wb_session_view *sv = wb_session_view_create(session);
        int rc = wb_session_view_create_slot(sv, 0, 0);
        if (rc == 0) PASS();
        else FAIL("wb_session_view_create_slot returned non-zero");
        wb_session_view_destroy(sv);
    }

    /* ---- Test 2: Launch clip ---- */
    TEST("Launch clip");
    {
        wb_session_view *sv = wb_session_view_create(session);
        wb_session_view_create_slot(sv, 0, 0);
        wb_session_view_set_slot_clip(sv, 0, 0, 5); /* clip_ref = 5 */
        int rc = wb_session_view_launch_clip(sv, 0, 0);
        int playing = wb_session_view_get_playing_scene(sv, 0);
        if (rc == 0 && playing == 0) PASS();
        else {
            char buf[128];
            snprintf(buf, sizeof(buf), "rc=%d playing_scene=%d", rc, playing);
            FAIL(buf);
        }
        wb_session_view_destroy(sv);
    }

    /* ---- Test 3: Stop clip ---- */
    TEST("Stop clip");
    {
        wb_session_view *sv = wb_session_view_create(session);
        wb_session_view_create_slot(sv, 0, 0);
        wb_session_view_set_slot_clip(sv, 0, 0, 3);
        wb_session_view_launch_clip(sv, 0, 0);
        int rc = wb_session_view_stop_clip(sv, 0);
        int playing = wb_session_view_get_playing_scene(sv, 0);
        if (rc == 0 && playing == -1) PASS();
        else {
            char buf[128];
            snprintf(buf, sizeof(buf), "rc=%d playing_scene=%d", rc, playing);
            FAIL(buf);
        }
        wb_session_view_destroy(sv);
    }

    /* ---- Test 4: Launch scene ---- */
    TEST("Launch scene");
    {
        wb_session_view *sv = wb_session_view_create(session);
        /* Set up slots on 3 tracks in scene 2 */
        for (int t = 0; t < 3; t++) {
            wb_session_view_create_slot(sv, t, 2);
            wb_session_view_set_slot_clip(sv, t, 2, t * 10 + 1);
        }
        int rc = wb_session_view_launch_scene(sv, 2);
        int s0 = wb_session_view_get_playing_scene(sv, 0);
        int s1 = wb_session_view_get_playing_scene(sv, 1);
        int s2 = wb_session_view_get_playing_scene(sv, 2);
        if (rc == 0 && s0 == 2 && s1 == 2 && s2 == 2) PASS();
        else {
            char buf[128];
            snprintf(buf, sizeof(buf), "rc=%d scenes=%d,%d,%d", rc, s0, s1, s2);
            FAIL(buf);
        }
        wb_session_view_destroy(sv);
    }

    /* ---- Test 5: Stop all ---- */
    TEST("Stop all");
    {
        wb_session_view *sv = wb_session_view_create(session);
        for (int t = 0; t < 3; t++) {
            wb_session_view_create_slot(sv, t, 0);
            wb_session_view_set_slot_clip(sv, t, 0, t);
            wb_session_view_launch_clip(sv, t, 0);
        }
        int rc = wb_session_view_stop_all(sv);
        int any_playing = 0;
        for (int t = 0; t < 3; t++) {
            if (wb_session_view_get_playing_scene(sv, t) >= 0) any_playing = 1;
        }
        if (rc == 0 && !any_playing) PASS();
        else {
            char buf[128];
            snprintf(buf, sizeof(buf), "rc=%d any_playing=%d", rc, any_playing);
            FAIL(buf);
        }
        wb_session_view_destroy(sv);
    }

    /* ---- Test 6: Launch mode affects behavior (toggle) ---- */
    TEST("Launch mode affects behavior (toggle)");
    {
        wb_session_view *sv = wb_session_view_create(session);
        wb_session_view_create_slot(sv, 0, 0);
        wb_session_view_set_slot_clip(sv, 0, 0, 7);
        /* Set toggle mode */
        wb_session_view_set_clip_launch_mode(sv, 0, WB_LAUNCH_TOGGLE);
        /* First launch: should start playing */
        wb_session_view_launch_clip(sv, 0, 0);
        int playing1 = wb_session_view_slot_playing(sv, 0, 0);
        /* Second launch (toggle): should stop */
        wb_session_view_launch_clip(sv, 0, 0);
        int playing2 = wb_session_view_slot_playing(sv, 0, 0);
        /* Third launch: should start again */
        wb_session_view_launch_clip(sv, 0, 0);
        int playing3 = wb_session_view_slot_playing(sv, 0, 0);
        if (playing1 == 1 && playing2 == 0 && playing3 == 1) PASS();
        else {
            char buf[128];
            snprintf(buf, sizeof(buf), "states=%d,%d,%d (expect 1,0,1)", playing1, playing2, playing3);
            FAIL(buf);
        }
        wb_session_view_destroy(sv);
    }

    /* ---- Test 7: Quantize setting stored ---- */
    TEST("Quantize setting stored");
    {
        wb_session_view *sv = wb_session_view_create(session);
        wb_session_view_create_slot(sv, 0, 0);
        int rc = wb_session_view_set_clip_quantize(sv, 0, WB_QUANT_1_16);
        int q = wb_session_view_get_slot_quantize(sv, 0, 0);
        if (rc == 0 && q == WB_QUANT_1_16) PASS();
        else {
            char buf[128];
            snprintf(buf, sizeof(buf), "rc=%d quantize=%d (expect 3)", rc, q);
            FAIL(buf);
        }
        wb_session_view_destroy(sv);
    }

    /* ---- Test 8: Playing clip tracking ---- */
    TEST("Playing clip tracking");
    {
        wb_session_view *sv = wb_session_view_create(session);
        wb_session_view_create_slot(sv, 0, 0);
        wb_session_view_set_slot_clip(sv, 0, 0, 42);
        wb_session_view_launch_clip(sv, 0, 0);
        /* Get the playing clip ref via the slot's stored ref */
        int clip = wb_session_view_get_slot_clip(sv, 0, 0);
        int is_playing = wb_session_view_slot_playing(sv, 0, 0);
        if (clip == 42 && is_playing) PASS();
        else {
            char buf[128];
            snprintf(buf, sizeof(buf), "clip=%d playing=%d (expect 42,1)", clip, is_playing);
            FAIL(buf);
        }
        wb_session_view_destroy(sv);
    }

    /* ---- Test 9: Scene launch with empty slots is no-op ---- */
    TEST("Scene launch with empty slots");
    {
        wb_session_view *sv = wb_session_view_create(session);
        /* Don't create any slots — scene is empty */
        int rc = wb_session_view_launch_scene(sv, 5);
        if (rc == -1) PASS(); /* no clips launched = -1 */
        else {
            char buf[128];
            snprintf(buf, sizeof(buf), "rc=%d (expect -1 for empty scene)", rc);
            FAIL(buf);
        }
        wb_session_view_destroy(sv);
    }

    /* ---- Test 10: Arrangement recording logs events ---- */
    TEST("Arrangement recording logs events");
    {
        wb_session_view *sv = wb_session_view_create(session);
        wb_session_view_record_to_arrangement(sv, 1);
        wb_session_view_create_slot(sv, 0, 0);
        wb_session_view_set_slot_clip(sv, 0, 0, 99);
        wb_session_view_advance_time(sv, 44100.0);
        wb_session_view_launch_clip(sv, 0, 0);
        wb_session_view_advance_time(sv, 22050.0);
        wb_session_view_stop_clip(sv, 0);
        uint32_t count = wb_session_view_arr_log_count(sv);
        if (count == 2) {
            const wb_arrangement_entry *e0 = wb_session_view_arr_log(sv, 0);
            const wb_arrangement_entry *e1 = wb_session_view_arr_log(sv, 1);
            if (e0 && e0->active == 1 && e0->clip_ref == 99 &&
                e1 && e1->active == 0 && e1->track == 0)
                PASS();
            else
                FAIL("log entries have wrong content");
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "log_count=%u (expect 2)", count);
            FAIL(buf);
        }
        wb_session_view_destroy(sv);
    }

    /* ---- Test 11: Invalid track/scene rejected ---- */
    TEST("Invalid track/scene rejected");
    {
        wb_session_view *sv = wb_session_view_create(session);
        int rc1 = wb_session_view_create_slot(sv, -1, 0);
        int rc2 = wb_session_view_create_slot(sv, 0, -1);
        int rc3 = wb_session_view_create_slot(sv, 999, 0);
        int rc4 = wb_session_view_create_slot(sv, 0, 999);
        if (rc1 == -1 && rc2 == -1 && rc3 == -1 && rc4 == -1)
            PASS();
        else {
            char buf[128];
            snprintf(buf, sizeof(buf), "rcs=%d,%d,%d,%d (expect all -1)", rc1, rc2, rc3, rc4);
            FAIL(buf);
        }
        wb_session_view_destroy(sv);
    }

    /* ---- Test 12: wb_session* convenience API works ---- */
    TEST("wb_session* convenience API works end-to-end");
    {
        /* Uses the default view behind the wb_session* API */
        wb_session_create_slot(session, 1, 3);
        int rc = wb_session_set_clip_launch_mode(session, 1, WB_LAUNCH_GATE);
        int qrc = wb_session_set_clip_quantize(session, 1, WB_QUANT_1_8);
        if (rc == 0 && qrc == 0) PASS();
        else {
            char buf[128];
            snprintf(buf, sizeof(buf), "rc=%d qrc=%d", rc, qrc);
            FAIL(buf);
        }
    }

    printf("\n==== %d/%d tests passed ====\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}