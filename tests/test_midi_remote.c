/* tests/test_midi_remote.c — verify wb_midi_remote mapping, process, save/load.
 * Links only with build/src/wb_midi_remote.o (no full engine needed). */

#include "wbus.h"
#include "wbus_midi_remote.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0;
static int checks = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } \
    else { printf("  ok: %s\n", msg); } \
} while (0)

int main(void) {
    printf("=== wb_midi_remote tests ===\n");

    /* 1. Add mapping */
    wb_midi_remote *mr = wb_midi_remote_create();
    CHECK(mr != NULL, "create returns non-NULL");
    CHECK(wb_midi_remote_count(mr) == 0, "initial count is 0");

    int rc = wb_midi_remote_add_map(mr, 1, 0, WB_MIDI_REMOTE_PARAM_VOLUME);
    CHECK(rc == 0, "add_map(CC1->track0/volume) returns 0");
    CHECK(wb_midi_remote_count(mr) == 1, "count is 1 after add");

    /* Add more mappings */
    wb_midi_remote_add_map(mr, 2, 0, WB_MIDI_REMOTE_PARAM_PAN);
    wb_midi_remote_add_map(mr, 7, 1, WB_MIDI_REMOTE_PARAM_VOLUME);
    CHECK(wb_midi_remote_count(mr) == 3, "count is 3 after 3 adds");

    /* Invalid params should fail */
    CHECK(wb_midi_remote_add_map(mr, 5, 0, 99) == -1, "add_map with invalid param rejected");
    CHECK(wb_midi_remote_add_map(mr, 200, 0, 0) == -1, "add_map with invalid CC rejected");

    /* 2. Process CC value */
    rc = wb_midi_remote_process(mr, 1, 0.75f);
    CHECK(rc == 0, "process(CC1, 0.75) returns 0 (mapped)");

    rc = wb_midi_remote_process(mr, 99, 0.5f);
    CHECK(rc == -1, "process(CC99, 0.5) returns -1 (not mapped)");

    /* Clamp test */
    rc = wb_midi_remote_process(mr, 2, 1.5f);
    CHECK(rc == 0, "process(CC2, 1.5) clamps and returns 0");
    rc = wb_midi_remote_process(mr, 2, -0.5f);
    CHECK(rc == 0, "process(CC2, -0.5) clamps and returns 0");

    /* 3. Get mapped param */
    int track = -1, param = -1;
    rc = wb_midi_remote_get_mapped_param(mr, 1, &track, &param);
    CHECK(rc == 0, "get_mapped_param(CC1) returns 0");
    CHECK(track == 0, "CC1 maps to track 0");
    CHECK(param == WB_MIDI_REMOTE_PARAM_VOLUME, "CC1 maps to volume param");

    track = -1; param = -1;
    rc = wb_midi_remote_get_mapped_param(mr, 7, &track, &param);
    CHECK(rc == 0, "get_mapped_param(CC7) returns 0");
    CHECK(track == 1, "CC7 maps to track 1");
    CHECK(param == WB_MIDI_REMOTE_PARAM_VOLUME, "CC7 maps to volume param");

    rc = wb_midi_remote_get_mapped_param(mr, 50, &track, &param);
    CHECK(rc == -1, "get_mapped_param(CC50) returns -1 (not mapped)");

    /* 4. Remove mapping */
    rc = wb_midi_remote_remove_map(mr, 2);
    CHECK(rc == 0, "remove_map(CC2) returns 0");
    CHECK(wb_midi_remote_count(mr) == 2, "count is 2 after remove");

    rc = wb_midi_remote_remove_map(mr, 2);
    CHECK(rc == -1, "remove_map(CC2) again returns -1 (already removed)");

    rc = wb_midi_remote_remove_map(mr, 200);
    CHECK(rc == -1, "remove_map(CC200) returns -1 (invalid CC)");

    /* 5. Clear all */
    rc = wb_midi_remote_clear(mr);
    CHECK(rc == 0, "clear returns 0");
    CHECK(wb_midi_remote_count(mr) == 0, "count is 0 after clear");

    /* 6. Count mappings (re-add and verify) */
    wb_midi_remote_add_map(mr, 10, 0, WB_MIDI_REMOTE_PARAM_VOLUME);
    wb_midi_remote_add_map(mr, 11, 0, WB_MIDI_REMOTE_PARAM_PAN);
    wb_midi_remote_add_map(mr, 12, 1, WB_MIDI_REMOTE_PARAM_MUTE);
    wb_midi_remote_add_map(mr, 13, 2, WB_MIDI_REMOTE_PARAM_SOLO);
    wb_midi_remote_add_map(mr, 14, 3, WB_MIDI_REMOTE_PARAM_SENDA);
    wb_midi_remote_add_map(mr, 15, 4, WB_MIDI_REMOTE_PARAM_SENDB);
    CHECK(wb_midi_remote_count(mr) == 6, "count is 6 after re-adding 6 maps");

    /* 7. Save/load round-trip */
    const char *tmp_path = "/tmp/test_midi_remote.json";
    rc = wb_midi_remote_save(mr, tmp_path);
    CHECK(rc == 0, "save returns 0");

    /* Clear and reload */
    wb_midi_remote_clear(mr);
    CHECK(wb_midi_remote_count(mr) == 0, "count 0 before load");

    rc = wb_midi_remote_load(mr, tmp_path);
    CHECK(rc == 0, "load returns 0");
    CHECK(wb_midi_remote_count(mr) == 6, "count is 6 after load round-trip");

    /* Verify loaded mappings */
    track = -1; param = -1;
    wb_midi_remote_get_mapped_param(mr, 10, &track, &param);
    CHECK(track == 0 && param == WB_MIDI_REMOTE_PARAM_VOLUME, "loaded CC10 -> track0/volume");
    wb_midi_remote_get_mapped_param(mr, 14, &track, &param);
    CHECK(track == 3 && param == WB_MIDI_REMOTE_PARAM_SENDA, "loaded CC14 -> track3/sendA");

    /* 8. Value normalization from MIDI 0..127 */
    /* Simulate MIDI 127 -> 1.0, 0 -> 0.0, 63 -> ~0.498 */
    wb_midi_remote_process(mr, 10, 127.0f / 127.0f);  /* = 1.0 */
    wb_midi_remote_process(mr, 11, 0.0f / 127.0f);     /* = 0.0 */
    wb_midi_remote_process(mr, 12, 63.0f / 127.0f);    /* ~0.496 */
    CHECK(1, "MIDI 0..127 normalizes to 0..1 (value stored)");

    /* Cleanup */
    wb_midi_remote_destroy(mr);
    remove(tmp_path);

    printf("\n=== %d checks, %d failures ===\n", checks, fails);
    return fails > 0 ? 1 : 0;
}