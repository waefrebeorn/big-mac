/* tests/test_project_templates.c — test project templates feature. */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "wbus.h"

int main(void) {
    int pass = 0, fail = 0;

    /* 1. Get template count */
    int count = wb_proj_template_count();
    if (count > 0) { printf("  PASS: template count = %d\n", count); pass++; }
    else { printf("  FAIL: template count <= 0\n"); fail++; }

    /* 2. Get name/description for each */
    int names_ok = 1;
    for (int i = 0; i < count; i++) {
        if (!wb_proj_template_get_name(i) || !wb_proj_template_get_description(i)) {
            names_ok = 0; break;
        }
    }
    if (names_ok) { printf("  PASS: all templates have name+description\n"); pass++; }
    else { printf("  FAIL: missing name/description\n"); fail++; }

    /* 3. Apply Songwriting template */
    wb_session *s = wb_session_create();
    if (wb_proj_template_apply(s, 1) == 0 && s->track_count >= 5) {
        printf("  PASS: Songwriting template has %u tracks\n", s->track_count); pass++;
    } else {
        printf("  FAIL: Songwriting template (tracks=%u)\n", s->track_count); fail++;
    }
    wb_session_destroy(s);

    /* 4. Apply Beat Making template — verify bus routing */
    s = wb_session_create();
    wb_proj_template_apply(s, 3);
    int has_bus = 0;
    for (uint32_t i = 0; i < s->track_count; i++) {
        if (s->tracks[i].kind == WB_TRACK_KIND_BUS) has_bus = 1;
    }
    if (has_bus) { printf("  PASS: Beat Making has bus tracks\n"); pass++; }
    else { printf("  FAIL: Beat Making missing buses\n"); fail++; }
    wb_session_destroy(s);

    /* 5. Apply Orchestral template — verify many tracks */
    s = wb_session_create();
    wb_proj_template_apply(s, 4);
    if (s->track_count >= 16) {
        printf("  PASS: Orchestral has %u tracks\n", s->track_count); pass++;
    } else {
        printf("  FAIL: Orchestral has %u tracks\n", s->track_count); fail++;
    }
    wb_session_destroy(s);

    /* 6. Apply Film Scoring template — verify markers */
    s = wb_session_create();
    wb_proj_template_apply(s, 6);
    if (s->marker_count > 0) {
        printf("  PASS: Film Scoring has %u markers\n", s->marker_count); pass++;
    } else {
        printf("  FAIL: Film Scoring has no markers\n"); fail++;
    }
    wb_session_destroy(s);

    /* 7. Apply YouTube Video template — verify video track */
    s = wb_session_create();
    wb_proj_template_apply(s, 8);
    int has_video = 0;
    for (uint32_t i = 0; i < s->track_count; i++) {
        if (s->tracks[i].kind == WB_TRACK_KIND_VIDEO) has_video = 1;
    }
    if (has_video) { printf("  PASS: YouTube Video has video track\n"); pass++; }
    else { printf("  FAIL: YouTube Video missing video track\n"); fail++; }
    wb_session_destroy(s);

    /* 8. Invalid template_id returns error */
    s = wb_session_create();
    if (wb_proj_template_apply(s, 999) == -1) { printf("  PASS: invalid template rejected\n"); pass++; }
    else { printf("  FAIL: invalid template accepted\n"); fail++; }
    wb_session_destroy(s);

    /* 9. NULL session returns error */
    if (wb_proj_template_apply(NULL, 0) == -1) { printf("  PASS: NULL session rejected\n"); pass++; }
    else { printf("  FAIL: NULL session accepted\n"); fail++; }

    /* 10. Idempotent (apply twice) */
    s = wb_session_create();
    wb_proj_template_apply(s, 0);
    int count1 = s->track_count;
    wb_proj_template_apply(s, 0);
    int count2 = s->track_count;
    if (count1 == count2) { printf("  PASS: idempotent apply\n"); pass++; }
    else { printf("  FAIL: not idempotent (%d vs %d)\n", count1, count2); fail++; }
    wb_session_destroy(s);

    printf("\nProject Templates: %d/%d passed\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
