/* tests/test_arrange_ai.c — test AI arrangement assistant. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

/* Verify that out_order is a permutation of [0..num_clips-1] */
static int is_permutation(int num_clips, const int *out_order) {
    if (num_clips <= 0) return 0;
    int seen[256];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < num_clips; i++) {
        if (out_order[i] < 0 || out_order[i] >= num_clips) return 0;
        if (seen[out_order[i]]) return 0;  /* duplicate */
        seen[out_order[i]] = 1;
    }
    return 1;
}

/* Verify every clip index appears exactly once in out_order */
static int all_clips_assigned(int num_clips, const int *out_order) {
    return is_permutation(num_clips, out_order);
}

int main(void) {
    printf("=== AI Arrangement Assistant Tests ===\n");

    /* 1. Create / destroy */
    void *a = wb_arrange_ai_create();
    CHECK(a != NULL);
    wb_arrange_ai_destroy(a);
    /* destroy should not crash on valid handle */
    CHECK(1);

    /* 2. Arrange pop style */
    a = wb_arrange_ai_create();
    int durations_pop[] = {8000, 16000, 12000, 20000, 10000, 18000, 14000, 6000};
    int num_pop = (int)(sizeof(durations_pop)/sizeof(durations_pop[0]));
    int order_pop[8], sections_pop[8];
    int rc = wb_arrange_ai_arrange(a, "pop", num_pop, durations_pop, order_pop, sections_pop);
    CHECK(rc == 0);
    /* pop has 8 sections in template; with 8 clips, each gets one template section */
    CHECK(sections_pop[0] == 0); /* intro */
    CHECK(sections_pop[2] == 2); /* chorus */
    CHECK(sections_pop[7] == 5); /* outro */

    /* 3. Arrange EDM style */
    int durations_edm[] = {4000, 8000, 5000, 10000, 6000, 9000, 3000};
    int num_edm = (int)(sizeof(durations_edm)/sizeof(durations_edm[0]));
    int order_edm[7], sections_edm[7];
    rc = wb_arrange_ai_arrange(a, "edm", num_edm, durations_edm, order_edm, sections_edm);
    CHECK(rc == 0);
    /* EDM template: build(6)-drop(7)-build(6)-drop(7)-breakdown(8)-drop(7)-outro(5) */
    CHECK(sections_edm[0] == 6); /* build */
    CHECK(sections_edm[1] == 7); /* drop */
    CHECK(sections_edm[4] == 8); /* breakdown */
    CHECK(sections_edm[6] == 5); /* outro */

    /* 4. Section names valid */
    char name[32];
    rc = wb_arrange_ai_get_section_name(0, name, sizeof(name));
    CHECK(rc == 0);
    CHECK(strcmp(name, "intro") == 0);
    rc = wb_arrange_ai_get_section_name(2, name, sizeof(name));
    CHECK(rc == 0);
    CHECK(strcmp(name, "chorus") == 0);
    rc = wb_arrange_ai_get_section_name(7, name, sizeof(name));
    CHECK(rc == 0);
    CHECK(strcmp(name, "drop") == 0);
    rc = wb_arrange_ai_get_section_name(9, name, sizeof(name));
    CHECK(rc == 0);
    CHECK(strcmp(name, "head") == 0);
    /* invalid section id */
    rc = wb_arrange_ai_get_section_name(99, name, sizeof(name));
    CHECK(rc == -1);
    /* bad buffer */
    rc = wb_arrange_ai_get_section_name(0, NULL, 0);
    CHECK(rc == -1);

    /* 5. All clips assigned (permutation check) */
    CHECK(all_clips_assigned(num_pop, order_pop));
    CHECK(all_clips_assigned(num_edm, order_edm));

    /* 6. Order is permutation of input */
    CHECK(is_permutation(num_pop, order_pop));
    CHECK(is_permutation(num_edm, order_edm));

    /* 7. Tempo set/get does not crash */
    wb_arrange_ai_set_tempo(a, 140.0f);
    wb_arrange_ai_set_tempo(a, -1.0f);   /* clamp */
    wb_arrange_ai_set_tempo(a, 1000.0f); /* clamp */
    wb_arrange_ai_set_tempo(a, 90.0f);
    CHECK(1);

    /* 8. All styles work */
    const char *style_names[] = {"pop", "rock", "edm", "hiphop", "jazz"};
    for (int s = 0; s < 5; s++) {
        int dur[8] = {1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000};
        int ord[8], sec[8];
        rc = wb_arrange_ai_arrange(a, style_names[s], 8, dur, ord, sec);
        CHECK(rc == 0);
        int perm = is_permutation(8, ord);
        CHECK(perm);
    }

    /* 9. Invalid style returns error */
    rc = wb_arrange_ai_arrange(a, "invalid_style", 4, durations_pop, order_pop, sections_pop);
    CHECK(rc == -1);

    /* 10. NULL args return error */
    rc = wb_arrange_ai_arrange(NULL, "pop", 4, durations_pop, order_pop, sections_pop);
    CHECK(rc == -1);
    rc = wb_arrange_ai_arrange(a, NULL, 4, durations_pop, order_pop, sections_pop);
    CHECK(rc == -1);
    rc = wb_arrange_ai_arrange(a, "pop", 4, NULL, order_pop, sections_pop);
    CHECK(rc == -1);

    wb_arrange_ai_destroy(a);

    printf("\nArrange AI: %d/%d passed\n", checks - fails, checks);
    return fails > 0 ? 1 : 0;
}