/* tests/test_cloud_collab.c — test cloud collaboration feature. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    void *cc = wb_cloud_collab_create("test-room");
    CHECK(cc != NULL);

    /* 1. Join user */
    int rc = wb_cloud_collab_join(cc, "user1");
    CHECK(rc == 0);

    /* 2. Apply operation */
    rc = wb_cloud_collab_apply_op(cc, "user1", "{\"type\":\"add_track\",\"name\":\"Drums\"}");
    CHECK(rc == 0);

    /* 3. Get state */
    char state[1024];
    rc = wb_cloud_collab_get_state(cc, state, sizeof(state));
    CHECK(rc == 0);
    CHECK(strstr(state, "test-room") != NULL);

    /* 4. User count */
    wb_cloud_collab_join(cc, "user2");
    CHECK(wb_cloud_collab_user_count(cc) == 2);

    /* 5. Leave user */
    rc = wb_cloud_collab_leave(cc, "user1");
    CHECK(rc == 0);
    CHECK(wb_cloud_collab_user_count(cc) == 1);

    /* 6. Max users */
    wb_cloud_collab_join(cc, "u3");
    wb_cloud_collab_join(cc, "u4");
    wb_cloud_collab_join(cc, "u5");
    wb_cloud_collab_join(cc, "u6");
    wb_cloud_collab_join(cc, "u7");
    wb_cloud_collab_join(cc, "u8");
    rc = wb_cloud_collab_join(cc, "u9"); /* should fail (max 8) */
    CHECK(rc == -1);

    wb_cloud_collab_destroy(cc);

    printf("\nCloud Collab: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}
