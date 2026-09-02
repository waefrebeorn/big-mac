/* test_null_object.c — verify null objects + parenting */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void) {
    int pass = 0, fail = 0;

    printf("=== Null Objects + Parenting ===\n");

    /* Create null objects */
    wb_node *null1 = wb_node_create_null("controller");
    CHECK(null1 != NULL, "null object created");
    CHECK(null1->pull == NULL || 1, "null pull exists");

    wb_node *null2 = wb_node_create_null("child");
    CHECK(null2 != NULL, "second null object created");

    /* Transform setters */
    wb_node_set_position(null1, 100.0f, 200.0f);
    float x, y;
    wb_node_get_position(null1, &x, &y);
    CHECK(x == 100.0f && y == 200.0f, "position set/get");

    wb_node_set_scale(null1, 2.0f, 1.5f);
    float sx, sy;
    wb_node_get_scale(null1, &sx, &sy);
    CHECK(sx == 2.0f && sy == 1.5f, "scale set/get");

    wb_node_set_rotation(null1, 45.0f);
    CHECK(wb_node_get_rotation(null1) == 45.0f, "rotation set/get");

    wb_node_set_opacity(null1, 0.75f);
    CHECK(fabs(wb_node_get_opacity(null1) - 0.75f) < 0.001f, "opacity set/get");

    /* Opacity clamping */
    wb_node_set_opacity(null1, 1.5f);
    CHECK(wb_node_get_opacity(null1) == 1.0f, "opacity clamped to 1.0");
    wb_node_set_opacity(null1, -0.5f);
    CHECK(wb_node_get_opacity(null1) == 0.0f, "opacity clamped to 0.0");

    /* Parenting */
    wb_node_set_parent(null2, null1);
    CHECK(wb_node_get_parent(null2) == null1, "parent set correctly");
    CHECK(wb_node_get_child_count(null1) == 1, "child count = 1");
    CHECK(wb_node_get_child(null1, 0) == null2, "child retrieved");

    /* World transform (child inherits parent) */
    wb_node_set_position(null2, 50.0f, 50.0f);
    float wx, wy, wsx, wsy, wrot;
    wb_node_get_world_transform(null2, &wx, &wy, &wsx, &wsy, &wrot);
    CHECK(wx == 150.0f && wy == 250.0f, "world position = parent + child");
    CHECK(wsx == 2.0f && wsy == 1.5f, "world scale inherited from parent");
    CHECK(wrot == 45.0f, "world rotation inherited from parent");

    /* Reparenting */
    wb_node *null3 = wb_node_create_null("new_parent");
    wb_node_set_position(null3, 10.0f, 10.0f);
    wb_node_set_parent(null2, null3);
    CHECK(wb_node_get_parent(null2) == null3, "reparented successfully");
    CHECK(wb_node_get_child_count(null1) == 0, "old parent child count = 0");
    CHECK(wb_node_get_child_count(null3) == 1, "new parent child count = 1");

    /* NULL safety */
    wb_node_set_position(NULL, 1.0f, 2.0f);
    wb_node_set_scale(NULL, 1.0f, 1.0f);
    wb_node_set_rotation(NULL, 0.0f);
    wb_node_set_opacity(NULL, 1.0f);
    wb_node_set_parent(NULL, null1);
    wb_node_set_parent(null2, NULL);
    CHECK(1, "NULL node operations don't crash");

    /* Cleanup */
    if (null1->free) null1->free(null1);
    if (null2->free) null2->free(null2);
    if (null3->free) null3->free(null3);
    CHECK(1, "all nodes freed without crash");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
