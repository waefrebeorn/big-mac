/* test_cloud.c — gate test for wb_cloud (cloud project sync with versioning).
 * Verifies: init, save/load, list, version count, restore, cleanup, delete. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wbus.h"

/* Cloud API declarations (from wbus.h). */
extern int wb_cloud_init(void);
extern int wb_cloud_save_project(const char *project_name, const wb_session *s);
extern wb_session *wb_cloud_load_project(const char *project_name, int version, int *out_version);
extern int wb_cloud_list_projects(char **names_out, int max_count);
extern int wb_cloud_delete_project(const char *project_name);
extern int wb_cloud_get_version_count(const char *project_name);
extern int wb_cloud_restore_version(const char *project_name, int version);
extern int wb_cloud_cleanup(const char *project_name, int keep_count);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

/* Helper: build a simple session with a track and a note. */
static wb_session *make_test_session(const char *name, int pitch) {
    wb_session *s = wb_session_create();
    if (!s) return NULL;
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->bpm = 120.0;
    wb_track *tr = wb_session_add_track(s, "Lead", 0);
    if (!tr) { wb_session_destroy(s); return NULL; }
    wb_session_add_note(tr, 0.0, 44100.0, pitch, 100);
    return s;
}

/* Helper: compare two sessions for equality (bpm, track count, first note pitch).
 * Note: wb_session_load sets the session name to the filename, so we CANNOT
 * compare names round-trip. Compare structural content instead. */
static int sessions_equal(const wb_session *a, const wb_session *b) {
    if (!a || !b) return 0;
    if (a->bpm != b->bpm) return 0;
    if (a->track_count != b->track_count) return 0;
    if (a->track_count == 0) return 1;
    const wb_track *ta = &a->tracks[0];
    const wb_track *tb = &b->tracks[0];
    if (ta->clip_count == 0 || tb->clip_count == 0) return 1;
    if (ta->clips[0].note_count == 0 || tb->clips[0].note_count == 0) return 1;
    if (ta->clips[0].notes[0].pitch != tb->clips[0].notes[0].pitch) return 0;
    return 1;
}

int main(void) {
    const char *proj = "cloudtest_project";

    /* Clean up any leftover from a previous run. */
    wb_cloud_delete_project(proj);

    /* ---- Test 1: wb_cloud_init ---- */
    TEST("wb_cloud_init creates cloud directory");
    if (wb_cloud_init() == 0) {
        PASS();
    } else {
        FAIL("wb_cloud_init returned non-zero");
    }

    /* ---- Test 2: wb_cloud_save_project ---- */
    TEST("wb_cloud_save_project saves first version (v0)");
    wb_session *s1 = make_test_session("My Song", 60);
    int v0 = wb_cloud_save_project(proj, s1);
    if (v0 == 0) {
        PASS();
    } else {
        printf("  [FAIL] expected version 0, got %d\n", v0);
    }

    /* ---- Test 3: wb_cloud_save_project again (v1) ---- */
    TEST("wb_cloud_save_project saves second version (v1)");
    wb_session *s2 = make_test_session("My Song v2", 64);
    int v1 = wb_cloud_save_project(proj, s2);
    if (v1 == 1) {
        PASS();
    } else {
        printf("  [FAIL] expected version 1, got %d\n", v1);
    }

    /* ---- Test 4: wb_cloud_get_version_count ---- */
    TEST("wb_cloud_get_version_count returns 2");
    int vc = wb_cloud_get_version_count(proj);
    if (vc == 2) {
        PASS();
    } else {
        printf("  [FAIL] expected 2, got %d\n", vc);
    }

    /* ---- Test 5: wb_cloud_load_project (specific version) ---- */
    TEST("wb_cloud_load_project loads version 0 correctly");
    int loaded_ver = -1;
    wb_session *loaded = wb_cloud_load_project(proj, 0, &loaded_ver);
    if (loaded && loaded_ver == 0 && sessions_equal(loaded, s1)) {
        PASS();
    } else {
        printf("  [FAIL] loaded=%p ver=%d equal=%d\n", (void*)loaded, loaded_ver,
               loaded ? sessions_equal(loaded, s1) : 0);
    }
    if (loaded) wb_session_destroy(loaded);

    /* ---- Test 6: wb_cloud_load_project (current version, version=-1) ---- */
    TEST("wb_cloud_load_project loads current version when version=-1");
    loaded = wb_cloud_load_project(proj, -1, &loaded_ver);
    if (loaded && loaded_ver == 1 && sessions_equal(loaded, s2)) {
        PASS();
    } else {
        printf("  [FAIL] loaded=%p ver=%d equal=%d\n", (void*)loaded, loaded_ver,
               loaded ? sessions_equal(loaded, s2) : 0);
    }
    if (loaded) wb_session_destroy(loaded);

    /* ---- Test 7: wb_cloud_list_projects ---- */
    TEST("wb_cloud_list_projects finds our project");
    char *names[16];
    int count = wb_cloud_list_projects(names, 16);
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], proj) == 0) { found = 1; break; }
    }
    for (int i = 0; i < count; i++) free(names[i]);
    if (found && count >= 1) {
        PASS();
    } else {
        printf("  [FAIL] count=%d found=%d\n", count, found);
    }

    /* ---- Test 8: wb_cloud_restore_version ---- */
    TEST("wb_cloud_restore_version restores v0 as new version");
    int restored = wb_cloud_restore_version(proj, 0);
    if (restored == 2) {   /* new version index (was 2 versions, now 3) */
        /* Load the restored version and verify it matches v0 */
        wb_session *r = wb_cloud_load_project(proj, restored, NULL);
        if (r && sessions_equal(r, s1)) {
            PASS();
        } else {
            printf("  [FAIL] restored session doesn't match v0\n");
        }
        if (r) wb_session_destroy(r);
    } else {
        printf("  [FAIL] expected restored version 2, got %d\n", restored);
    }

    /* ---- Test 9: wb_cloud_cleanup ---- */
    TEST("wb_cloud_cleanup keeps only latest 2 versions");
    /* We now have 3 versions (0, 1, 2). Cleanup to keep 2. */
    int removed = wb_cloud_cleanup(proj, 2);
    if (removed == 1 && wb_cloud_get_version_count(proj) == 2) {
        PASS();
    } else {
        printf("  [FAIL] removed=%d vc=%d\n", removed, wb_cloud_get_version_count(proj));
    }

    /* ---- Test 10: wb_cloud_delete_project ---- */
    TEST("wb_cloud_delete_project removes the project");
    if (wb_cloud_delete_project(proj) == 0 && wb_cloud_get_version_count(proj) == 0) {
        PASS();
    } else {
        printf("  [FAIL] delete failed or version count not zero\n");
    }

    /* ---- Test 11: save/load with empty session ---- */
    TEST("wb_cloud handles empty session");
    wb_session *empty = wb_session_create();
    int ve = wb_cloud_save_project(proj, empty);
    wb_session *le = wb_cloud_load_project(proj, ve, NULL);
    /* wb_session_load sets name to filename, so just check it loaded with expected structure */
    int ok = (ve >= 0 && le != NULL && le->track_count == 0 && le->bpm == 120.0);
    if (ok) PASS(); else FAIL("empty session round-trip failed");
    if (le) wb_session_destroy(le);
    wb_session_destroy(empty);

    /* ---- Test 12: NULL/invalid inputs ---- */
    TEST("wb_cloud handles NULL inputs gracefully");
    int all_ok = 1;
    if (wb_cloud_save_project(NULL, s1) != -1) all_ok = 0;
    if (wb_cloud_save_project(proj, NULL) != -1) all_ok = 0;
    if (wb_cloud_load_project(NULL, 0, NULL) != NULL) all_ok = 0;
    if (wb_cloud_delete_project(NULL) != -1) all_ok = 0;
    if (wb_cloud_get_version_count(NULL) != 0) all_ok = 0;
    if (wb_cloud_restore_version(NULL, 0) != -1) all_ok = 0;
    if (wb_cloud_cleanup(NULL, 1) != -1) all_ok = 0;
    if (all_ok) PASS(); else FAIL("NULL input handling incorrect");

    /* Cleanup */
    wb_cloud_delete_project(proj);
    wb_session_destroy(s1);
    wb_session_destroy(s2);

    /* ---- summary ---- */
    printf("\n=== Cloud Sync Tests: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}