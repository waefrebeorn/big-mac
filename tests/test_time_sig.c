/* tests/test_time_sig.c — time signature changes feature tests.
 * Verifies the sorted time-sig map, bar/beat conversions, and validation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus.h"

static int failures = 0;
static int checks = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

#define EPS 1.0  /* 1 sample tolerance for position comparisons */

int main(void) {
    printf("=== time signature changes ===\n");

    /* Test 1: Default time sig is 4/4 */
    wb_session *s = wb_session_create();
    CHECK(s != NULL, "session created");
    CHECK(s->time_sig_num == 4 && s->time_sig_den == 4, "default time sig is 4/4");

    int num = 0, den = 0;
    wb_session_get_time_sig_at(s, 0.0, &num, &den);
    CHECK(num == 4 && den == 4, "get_time_sig_at at 0 returns 4/4 default");
    wb_session_get_time_sig_at(s, 1000.0, &num, &den);
    CHECK(num == 4 && den == 4, "get_time_sig_at at 1000 returns 4/4 default");

    /* Test 2: Add time sig change at bar index 4 (3/4) */
    /* At 120 BPM, 4/4: quarter = 0.5s = 22050 samples, bar = 88200 samples.
     * Bar index 4 starts at 4 * 88200 = 352800 samples. */
    double sr = WB_SAMPLE_RATE;
    double quarter = (60.0 / s->bpm) * sr;  /* 22050 */
    double bar4_4 = 4.0 * quarter;           /* 88200 */
    double change_pos = 4.0 * bar4_4;        /* 352800 = start of bar index 4 */

    int idx = wb_session_add_time_sig_change(s, change_pos, 3, 4);
    CHECK(idx == 0, "add 3/4 at bar 5 returns index 0");
    CHECK(wb_session_time_sig_change_count(s) == 1, "change count is 1");

    /* Before the change: still 4/4 */
    wb_session_get_time_sig_at(s, change_pos - 1.0, &num, &den);
    CHECK(num == 4 && den == 4, "before change point: 4/4");
    /* At the change: 3/4 */
    wb_session_get_time_sig_at(s, change_pos, &num, &den);
    CHECK(num == 3 && den == 4, "at change point: 3/4");
    /* After the change: 3/4 */
    wb_session_get_time_sig_at(s, change_pos + 1000.0, &num, &den);
    CHECK(num == 3 && den == 4, "after change point: 3/4");

    /* Test 3: Add another change at bar index 8 (7/8) */
    /* Bar index 8 in 4/4 = 8 * 88200 = 705600 */
    double change2_pos = 8.0 * bar4_4;  /* 705600 */
    idx = wb_session_add_time_sig_change(s, change2_pos, 7, 8);
    CHECK(idx == 1, "add 7/8 at bar index 8 returns index 1");
    CHECK(wb_session_time_sig_change_count(s) == 2, "change count is 2");

    /* At change2: 7/8 */
    wb_session_get_time_sig_at(s, change2_pos, &num, &den);
    CHECK(num == 7 && den == 8, "at change2: 7/8");
    /* Between the two changes: 3/4 */
    wb_session_get_time_sig_at(s, (change_pos + change2_pos) / 2.0, &num, &den);
    CHECK(num == 3 && den == 4, "between changes: 3/4");
    /* After change2: 7/8 */
    wb_session_get_time_sig_at(s, change2_pos + 1000.0, &num, &den);
    CHECK(num == 7 && den == 8, "after change2: 7/8");

    /* Test 4: Remove a change, verify it's gone */
    int ret = wb_session_remove_time_sig_change(s, 0);
    CHECK(ret == 0, "remove change at index 0 succeeds");
    CHECK(wb_session_time_sig_change_count(s) == 1, "change count is 1 after remove");
    /* Now only the 7/8 change remains (at index 0) */
    double pos_out = 0;
    wb_session_get_time_sig_change(s, 0, &pos_out, &num, &den);
    CHECK(fabs(pos_out - change2_pos) < EPS && num == 7 && den == 8,
          "remaining change is 7/8 at change2_pos");
    /* Before change2: now 4/4 (no more 3/4 change) */
    wb_session_get_time_sig_at(s, change_pos, &num, &den);
    CHECK(num == 4 && den == 4, "after remove, before change2: 4/4");

    /* Re-add the 3/4 change for further tests */
    wb_session_add_time_sig_change(s, change_pos, 3, 4);
    CHECK(wb_session_time_sig_change_count(s) == 2, "re-added 3/4, count is 2");

    /* Test 5: get_bar_start returns correct sample position */
    /* Bar 0 = 0, bar 1 = 88200, bar 2 = 176400, etc. (all 4/4 until bar 5) */
    double bs = wb_session_get_bar_start(s, 0);
    CHECK(fabs(bs - 0.0) < EPS, "bar 0 starts at 0");
    bs = wb_session_get_bar_start(s, 1);
    CHECK(fabs(bs - bar4_4) < EPS, "bar 1 starts at 88200");
    bs = wb_session_get_bar_start(s, 4);
    CHECK(fabs(bs - change_pos) < EPS, "bar 4 starts at 352800 (3/4 change)");
    /* Bar 5 is the second bar in 3/4: change_pos + one 3/4 bar */
    double bar3_4 = 3.0 * quarter;  /* 66150 */
    bs = wb_session_get_bar_start(s, 5);
    CHECK(fabs(bs - (change_pos + bar3_4)) < EPS, "bar 5 starts at 418950 (second 3/4 bar)");
    /* Bar 8: 4 bars of 4/4 + 4 bars of 3/4 = change_pos + 4*bar3_4 */
    double expected_bar8 = change_pos + 4.0 * bar3_4;  /* 617400 */
    bs = wb_session_get_bar_start(s, 8);
    CHECK(fabs(bs - expected_bar8) < EPS, "bar 8 starts at 617400 (7/8 change)");

    /* Test 6: get_bar_at returns correct bar number */
    int bar = wb_session_get_bar_at(s, 0.0);
    CHECK(bar == 0, "pos 0 -> bar 0");
    bar = wb_session_get_bar_at(s, bar4_4);
    CHECK(bar == 1, "pos 88200 -> bar 1");
    bar = wb_session_get_bar_at(s, change_pos);
    CHECK(bar == 4, "pos 352800 -> bar 4 (start of 3/4)");
    bar = wb_session_get_bar_at(s, change_pos + 1.0);
    CHECK(bar == 4, "pos 352801 -> bar 4");
    bar = wb_session_get_bar_at(s, expected_bar8);
    CHECK(bar == 8, "pos 617400 -> bar 8");

    /* Test 7: samples_to_beats and beats_to_samples round-trip */
    double beats = wb_session_samples_to_beats(s, quarter);
    CHECK(fabs(beats - 1.0) < 1e-9, "one quarter note = 1 beat");
    double samples = wb_session_beats_to_samples(s, 1.0);
    CHECK(fabs(samples - quarter) < EPS, "1 beat = 22050 samples");
    beats = wb_session_samples_to_beats(s, 4.0 * quarter);
    CHECK(fabs(beats - 4.0) < 1e-9, "4 quarter notes = 4 beats");
    samples = wb_session_beats_to_samples(s, 4.0);
    CHECK(fabs(samples - 4.0 * quarter) < EPS, "4 beats = 88200 samples");
    /* Round-trip */
    double original = 12345.67;
    double round_tripped = wb_session_beats_to_samples(s, wb_session_samples_to_beats(s, original));
    CHECK(fabs(round_tripped - original) < 1e-6, "samples->beats->samples round-trip");

    /* Test 8: Time sig changes are kept sorted by position */
    /* Add out of order: first at a later pos, then at an earlier pos */
    wb_session *s2 = wb_session_create();
    double pos_a = 500000.0, pos_b = 100000.0, pos_c = 300000.0;
    wb_session_add_time_sig_change(s2, pos_a, 5, 4);
    wb_session_add_time_sig_change(s2, pos_b, 6, 8);
    wb_session_add_time_sig_change(s2, pos_c, 7, 8);
    double p0, p1, p2;
    wb_session_get_time_sig_change(s2, 0, &p0, NULL, NULL);
    wb_session_get_time_sig_change(s2, 1, &p1, NULL, NULL);
    wb_session_get_time_sig_change(s2, 2, &p2, NULL, NULL);
    CHECK(fabs(p0 - pos_b) < EPS, "sorted[0] = earliest (100000)");
    CHECK(fabs(p1 - pos_c) < EPS, "sorted[1] = middle (300000)");
    CHECK(fabs(p2 - pos_a) < EPS, "sorted[2] = latest (500000)");

    /* Test 9: Invalid time sig (denominator not power of 2) is rejected */
    CHECK(wb_session_add_time_sig_change(s2, 50000.0, 4, 3) == -1, "den=3 rejected");
    CHECK(wb_session_add_time_sig_change(s2, 50000.0, 4, 5) == -1, "den=5 rejected");
    CHECK(wb_session_add_time_sig_change(s2, 50000.0, 4, 6) == -1, "den=6 rejected");
    CHECK(wb_session_add_time_sig_change(s2, 50000.0, 4, 7) == -1, "den=7 rejected");
    CHECK(wb_session_add_time_sig_change(s2, 50000.0, 4, 0) == -1, "den=0 rejected");
    CHECK(wb_session_add_time_sig_change(s2, 50000.0, 0, 4) == -1, "num=0 rejected");
    CHECK(wb_session_add_time_sig_change(s2, 50000.0, 33, 4) == -1, "num=33 rejected");
    CHECK(wb_session_add_time_sig_change(s2, 50000.0, 4, 64) == -1, "den=64 rejected");
    /* Valid ones accepted */
    CHECK(wb_session_add_time_sig_change(s2, 50000.0, 4, 1) >= 0, "den=1 accepted");
    CHECK(wb_session_add_time_sig_change(s2, 51000.0, 4, 2) >= 0, "den=2 accepted");
    CHECK(wb_session_add_time_sig_change(s2, 52000.0, 4, 8) >= 0, "den=8 accepted");
    CHECK(wb_session_add_time_sig_change(s2, 53000.0, 4, 16) >= 0, "den=16 accepted");
    CHECK(wb_session_add_time_sig_change(s2, 54000.0, 4, 32) >= 0, "den=32 accepted");

    /* Test 10: Multiple changes at different positions all resolve correctly */
    wb_session *s3 = wb_session_create();
    /* Add several changes */
    double p10 = 100000.0, p20 = 200000.0, p30 = 300000.0, p40 = 400000.0;
    wb_session_add_time_sig_change(s3, p10, 3, 4);
    wb_session_add_time_sig_change(s3, p20, 5, 4);
    wb_session_add_time_sig_change(s3, p30, 7, 8);
    wb_session_add_time_sig_change(s3, p40, 6, 8);
    CHECK(wb_session_time_sig_change_count(s3) == 4, "4 changes added");

    /* Check regions */
    wb_session_get_time_sig_at(s3, p10 - 1.0, &num, &den);
    CHECK(num == 4 && den == 4, "region before first change: 4/4");
    wb_session_get_time_sig_at(s3, p10, &num, &den);
    CHECK(num == 3 && den == 4, "at p10: 3/4");
    wb_session_get_time_sig_at(s3, p20 - 1.0, &num, &den);
    CHECK(num == 3 && den == 4, "just before p20: 3/4");
    wb_session_get_time_sig_at(s3, p20, &num, &den);
    CHECK(num == 5 && den == 4, "at p20: 5/4");
    wb_session_get_time_sig_at(s3, p30, &num, &den);
    CHECK(num == 7 && den == 8, "at p30: 7/8");
    wb_session_get_time_sig_at(s3, p40, &num, &den);
    CHECK(num == 6 && den == 8, "at p40: 6/8");
    wb_session_get_time_sig_at(s3, p40 + 1.0, &num, &den);
    CHECK(num == 6 && den == 8, "after p40: 6/8");

    /* Verify all changes readable */
    for (int i = 0; i < 4; i++) {
        double rp; int rn, rd;
        wb_session_get_time_sig_change(s3, i, &rp, &rn, &rd);
        if (i == 0) { CHECK(rn == 3 && rd == 4, "change[0] = 3/4"); }
        if (i == 1) { CHECK(rn == 5 && rd == 4, "change[1] = 5/4"); }
        if (i == 2) { CHECK(rn == 7 && rd == 8, "change[2] = 7/8"); }
        if (i == 3) { CHECK(rn == 6 && rd == 8, "change[3] = 6/8"); }
    }

    /* Edge: remove from invalid index */
    CHECK(wb_session_remove_time_sig_change(s3, -1) == -1, "remove index -1 fails");
    CHECK(wb_session_remove_time_sig_change(s3, 100) == -1, "remove index 100 fails");

    wb_session_destroy(s);
    wb_session_destroy(s2);
    wb_session_destroy(s3);

    printf("\n=== %d/%d checks passed ===\n", checks - failures, checks);
    return failures > 0 ? 1 : 0;
}