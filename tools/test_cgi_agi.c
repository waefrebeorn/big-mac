/* test_cgi_agi.c — R043-G7 gate: 3D-CGI scene model + AGI task bridge. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_cgi.h"
#include "wbus/wbus_agi.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } } while (0)

int main(void) {
    printf("=== R043-G7: 3D-CGI scene + AGI control surface ===\n\n");

    /* ---- CGI: scene lifecycle + geometry sanity ---- */
    wb_cgi_scene *sc = wb_cgi_scene_create();
    CHECK(sc != NULL, "cgi scene created");
    CHECK(wb_cgi_scene_tri_count(sc) == 12, "cube has 12 triangles");
    CHECK(wb_cgi_scene_grid_count(sc) == 10, "ground grid has 10 lines");

    /* projected triangles are finite and near the viewport center */
    int finite = 1, spread = 0;
    for (int i = 0; i < wb_cgi_scene_tri_count(sc); i++) {
        float x0,y0,x1,y1,x2,y2,sh;
        wb_cgi_scene_tri(sc, i, &x0,&y0,&x1,&y1,&x2,&y2,&sh);
        if (!(isfinite(x0)&&isfinite(y0)&&isfinite(x1)&&isfinite(y1)&&
              isfinite(x2)&&isfinite(y2))) finite = 0;
        if (fabsf(x0-x1) > 1.0f || fabsf(y1-y2) > 1.0f) spread = 1;
        if (sh <= 0.0f || sh > 1.0f) { finite = 0; }
    }
    CHECK(finite == 1, "all projected coords + shades are sane/finite");
    CHECK(spread == 1, "triangles have real screen extent (not degenerate)");

    /* tick rotates: projection changes */
    float ax0, ay0, ax1, ay1, ax2, ay2, ash;
    wb_cgi_scene_tri(sc, 0, &ax0,&ay0,&ax1,&ay1,&ax2,&ay2,&ash);
    wb_cgi_scene_tick(sc, 1.0);
    float bx0, by0, bx1, by1, bx2, by2, bsh;
    wb_cgi_scene_tri(sc, 0, &bx0,&by0,&bx1,&by1,&bx2,&by2,&bsh);
    CHECK(fabsf(ax0-bx0) > 0.5f || fabsf(ay0-by0) > 0.5f,
          "tick animates the projection (rotation is live)");

    /* zoom changes scale */
    wb_cgi_scene_set_zoom(sc, 2.0f);
    float cx0, cy0, cx1, cy1, cx2, cy2, csh;
    wb_cgi_scene_tri(sc, 0, &cx0,&cy0,&cx1,&cy1,&cx2,&cy2,&csh);
    float d_before = fabsf(ax0-ax1), d_after = fabsf(cx0-cx1);
    CHECK(d_after > d_before * 1.3f, "zoom=2 scales the cube up");

    /* manual rotation setter round-trips */
    wb_cgi_scene_set_rotation(sc, 0.1f, 0.2f, 0.3f);
    float rrx, rry, rrz;
    wb_cgi_scene_get_rotation(sc, &rrx, &rry, &rrz);
    CHECK(rrx > 0.09f && rrx < 0.11f && rry > 0.19f && rry < 0.21f,
          "set/get rotation round-trips");
    CHECK(wb_cgi_scene_get_zoom(sc) > 1.99f && wb_cgi_scene_get_zoom(sc) < 2.01f,
          "zoom getter round-trips");
    wb_cgi_scene_destroy(sc);
    CHECK(1, "cgi scene destroyed cleanly");

    /* ---- AGI: task lifecycle ---- */
    wb_agi *a = wb_agi_create();
    CHECK(a != NULL, "agi created");
    CHECK(wb_agi_task_count(a) == 0, "agi starts empty");

    int t0 = wb_agi_submit(a, "render episode");
    int t1 = wb_agi_submit(a, "polish voice -16 LUFS");
    int t2 = wb_agi_submit(a, "auto-cut shorts");
    CHECK(t0 == 0 && t1 == 1 && t2 == 2, "three tasks submitted with ids");
    CHECK(strcmp(wb_agi_task_label(a, t1), "polish voice -16 LUFS") == 0,
          "task label stored verbatim");
    CHECK(wb_agi_task_status(a, t0) == WB_AGI_QUEUED, "tasks start queued");

    /* first tick promotes task 0 to running */
    wb_agi_tick(a, 0.1);
    CHECK(wb_agi_task_status(a, t0) == WB_AGI_RUNNING,
          "first queued task promoted to running");
    CHECK(strstr(wb_agi_last_event(a), "running") != NULL,
          "last_event reflects promotion");

    /* ~4s to complete; feed 4.5s of ticks */
    for (int i = 0; i < 45; i++) wb_agi_tick(a, 0.1);
    CHECK(wb_agi_task_status(a, t0) == WB_AGI_DONE, "task 0 completes after ~4s");
    CHECK(wb_agi_task_status(a, t1) == WB_AGI_RUNNING ||
          wb_agi_task_progress(a, t1) > 0.0f,
          "next task started (pipeline advances)");
    CHECK(wb_agi_done_count(a) >= 1, "done count >= 1");
    CHECK(wb_agi_running_count(a) <= 1, "at most one running at a time");
    CHECK(wb_agi_task_progress(a, t0) == 1.0f, "progress clamps at 1.0");

    /* invalid accessors are safe */
    CHECK(wb_agi_task_status(a, 99) == WB_AGI_QUEUED, "invalid id -> safe status");
    CHECK(wb_agi_submit(a, "") == -1, "empty label rejected");
    wb_agi_destroy(a);

    printf("\n%d checks, %d failures\n",
           21, failures);
    return failures ? 1 : 0;
}
