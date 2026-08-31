/* tests/test_mod_matrix.c — modulation matrix tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus.h"

static int checks = 0, fails = 0;
#define CHECK(c) do { checks++; if (c) printf("  PASS: " #c "\n"); else { printf("  FAIL: " #c "\n"); fails++; } } while(0)

int main(void) {
    printf("=== Modulation Matrix Tests ===\n\n");

    /* 1. Create/destroy */
    printf("[1] Create/destroy:\n");
    void *mm = wb_mod_matrix_create();
    CHECK(mm != NULL);
    wb_mod_matrix_destroy(mm);
    printf("\n");

    /* 2. Add route */
    printf("[2] Add route:\n");
    mm = wb_mod_matrix_create();
    CHECK(mm != NULL);

    int r1 = wb_mod_matrix_add_route(mm, 0, 5, 0.5f);   /* LFO1 -> param 5 */
    CHECK(r1 > 0);

    int r2 = wb_mod_matrix_add_route(mm, 2, 10, -0.3f); /* Env -> param 10 */
    CHECK(r2 > 0);
    CHECK(r2 != r1);  /* unique IDs */

    /* Invalid source */
    int rbad = wb_mod_matrix_add_route(mm, 99, 0, 0.5f);
    CHECK(rbad < 0);

    /* Invalid destination */
    rbad = wb_mod_matrix_add_route(mm, 0, 200, 0.5f);
    CHECK(rbad < 0);

    /* Invalid amount */
    rbad = wb_mod_matrix_add_route(mm, 0, 0, 2.0f);
    CHECK(rbad < 0);

    printf("\n");

    /* 3. Process modulates params */
    printf("[3] Process modulates params:\n");
    wb_mod_matrix_clear(mm);

    /* Set up a static source: velocity */
    wb_mod_matrix_set_midi(mm, 0.8f, 0.0f, 0.0f, 0.0f);

    /* Route velocity -> param 0 with amount 1.0 */
    r1 = wb_mod_matrix_add_route(mm, 3, 0, 1.0f);  /* Velocity source = 3 */
    CHECK(r1 > 0);

    float params[16];
    memset(params, 0, sizeof(params));
    params[0] = 0.5f;  /* base value */

    wb_mod_matrix_process(mm, params, 16);
    /* velocity 0.8 * amount 1.0 = 0.8 added to param 0 */
    CHECK(fabsf(params[0] - 1.3f) < 0.01f);

    /* Now test with LFO (oscillating source) */
    wb_mod_matrix_clear(mm);
    wb_mod_matrix_set_lfo(mm, 0, 0, 10.0f);  /* LFO1 sine, 10Hz */
    r1 = wb_mod_matrix_add_route(mm, 0, 7, 0.5f);  /* LFO1 -> param 7 */
    CHECK(r1 > 0);

    memset(params, 0, sizeof(params));
    /* Process enough samples — LFO should produce a non-zero modulation */
    int modulated = 0;
    for (int i = 0; i < 1000; i++) {
        float p[16] = {0};
        p[7] = 0.0f;
        wb_mod_matrix_process(mm, p, 16);
        if (fabsf(p[7]) > 0.01f) {
            modulated = 1;
            break;
        }
    }
    CHECK(modulated);

    printf("\n");

    /* 4. Remove route */
    printf("[4] Remove route:\n");
    wb_mod_matrix_clear(mm);

    r1 = wb_mod_matrix_add_route(mm, 0, 0, 0.5f);
    r2 = wb_mod_matrix_add_route(mm, 1, 1, 0.5f);
    CHECK(wb_mod_matrix_route_count(mm) == 2);

    int ret = wb_mod_matrix_remove_route(mm, r1);
    CHECK(ret == 0);
    CHECK(wb_mod_matrix_route_count(mm) == 1);

    /* Removing same ID again should fail */
    ret = wb_mod_matrix_remove_route(mm, r1);
    CHECK(ret < 0);

    /* Invalid route ID */
    ret = wb_mod_matrix_remove_route(mm, -1);
    CHECK(ret < 0);

    printf("\n");

    /* 5. Clear all */
    printf("[5] Clear all:\n");
    wb_mod_matrix_clear(mm);
    CHECK(wb_mod_matrix_route_count(mm) == 0);

    /* Add multiple routes */
    for (int i = 0; i < 10; i++) {
        wb_mod_matrix_add_route(mm, i % 7, i, 0.1f * (float)(i + 1));
    }
    CHECK(wb_mod_matrix_route_count(mm) == 10);

    wb_mod_matrix_clear(mm);
    CHECK(wb_mod_matrix_route_count(mm) == 0);

    printf("\n");

    /* 6. Route count */
    printf("[6] Route count:\n");
    wb_mod_matrix_clear(mm);
    CHECK(wb_mod_matrix_route_count(mm) == 0);

    /* Add routes one by one and track their IDs */
    int ids[10];
    for (int i = 0; i < 5; i++) {
        ids[i] = wb_mod_matrix_add_route(mm, 0, i, 0.5f);
        CHECK(ids[i] > 0);
        CHECK(wb_mod_matrix_route_count(mm) == i + 1);
    }

    /* Remove first route by its actual ID */
    wb_mod_matrix_remove_route(mm, ids[0]);
    CHECK(wb_mod_matrix_route_count(mm) == 4);

    /* Fill to max */
    wb_mod_matrix_clear(mm);
    int added = 0;
    for (int i = 0; i < 70; i++) {  /* try more than max */
        int r = wb_mod_matrix_add_route(mm, i % 7, i % 128, 0.01f);
        if (r > 0) added++;
    }
    CHECK(added == 64);  /* capped at WB_MOD_MATRIX_MAX_ROUTES */
    CHECK(wb_mod_matrix_route_count(mm) == 64);

    printf("\n");

    /* 7. Set amount */
    printf("[7] Set amount:\n");
    wb_mod_matrix_clear(mm);
    wb_mod_matrix_set_midi(mm, 1.0f, 0.0f, 0.0f, 0.0f);  /* velocity = 1.0 */
    r1 = wb_mod_matrix_add_route(mm, 3, 0, 0.0f);  /* zero amount */
    CHECK(r1 > 0);

    params[0] = 0.0f;
    wb_mod_matrix_process(mm, params, 16);
    CHECK(fabsf(params[0]) < 0.001f);  /* no modulation */

    wb_mod_matrix_set_amount(mm, r1, 1.0f);
    params[0] = 0.0f;
    wb_mod_matrix_process(mm, params, 16);
    CHECK(fabsf(params[0] - 1.0f) < 0.01f);  /* full modulation */

    wb_mod_matrix_set_amount(mm, r1, -1.0f);
    params[0] = 0.0f;
    wb_mod_matrix_process(mm, params, 16);
    CHECK(fabsf(params[0] + 1.0f) < 0.01f);  /* inverted modulation */

    /* Amount clamping */
    wb_mod_matrix_set_amount(mm, r1, 5.0f);  /* should clamp to 1.0 */
    params[0] = 0.0f;
    wb_mod_matrix_process(mm, params, 16);
    CHECK(params[0] <= 1.0f + 0.01f);

    printf("\n");

    /* 8. Envelope modulation */
    printf("[8] Envelope modulation:\n");
    wb_mod_matrix_clear(mm);
    wb_mod_matrix_note_on(mm, 0.001f, 0.001f, 0.8f, 0.001f);  /* fast ADSR */
    r1 = wb_mod_matrix_add_route(mm, 2, 0, 1.0f);  /* Envelope -> param 0 */
    CHECK(r1 > 0);

    /* Process enough samples for envelope to reach sustain */
    memset(params, 0, sizeof(params));
    for (int i = 0; i < 200; i++) {
        float p[16] = {0};
        wb_mod_matrix_process(mm, p, 16);
        params[0] = p[0];
    }
    /* Envelope should have reached near-sustain (0.8) */
    CHECK(params[0] > 0.5f);

    /* Note off — envelope should release */
    wb_mod_matrix_note_off(mm);
    for (int i = 0; i < 200; i++) {
        float p[16] = {0};
        wb_mod_matrix_process(mm, p, 16);
        params[0] = p[0];
    }
    CHECK(params[0] < 0.1f);

    printf("\n");

    wb_mod_matrix_destroy(mm);

    printf("=== Results: %d/%d checks passed", checks - fails, checks);
    if (fails > 0) printf(", %d FAILED", fails);
    printf(" ===\n");

    return fails > 0 ? 1 : 0;
}