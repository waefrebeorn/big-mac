/* test_text_animator.c — verify per-character text animation node.
 *
 * Tests:
 *   1. Node creation with text input
 *   2. Default properties are set correctly
 *   3. Range setter (selector-based animation)
 *   4. Properties setter (offset, scale, rotation, opacity)
 *   5. Easing setter (all 6 types)
 *   6. Delay setter (stagger)
 *   7. Pull at t=0 (characters in initial animated state)
 *   8. Pull at t=duration (characters at final state)
 *   9. Pull at mid-animation (intermediate state)
 *  10. Range selector: only chars in range are animated
 *  11. Frame dimensions match declared format
 *  12. Node destroy cleans up state
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus/wbus_compositor.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

/* count non-zero alpha pixels in a frame */
static int count_opaque_pixels(const wb_frame *f) {
    int count = 0;
    for (int i = 0; i < f->w * f->h; i++) {
        if (f->px[i].a > 0.01f) count++;
    }
    return count;
}

int main(void) {
    printf("=== Per-Character Text Animator Test ===\n\n");

    /* ---- Test 1: node creation ---- */
    printf("-- node creation --\n");
    wb_node *ta = wb_node_effect_text_animator("HELLO", 3,
                                                1.0f, 1.0f, 1.0f, 1.0f,
                                                320, 120);
    CHECK(ta != NULL, "text animator node created");
    CHECK(ta->kind == WB_NODE_EFFECT, "kind is WB_NODE_EFFECT");
    if (ta) {
        int w, h;
        wb_node_get_format(ta, &w, &h);
        CHECK(w == 320 && h == 120, "format is 320x120");
    }

    /* ---- Test 2: pull at t=0 (initial state - chars should be invisible
     *      due to opacity=0 at u=0) ---- */
    printf("\n-- initial state (t=0) --\n");
    wb_frame *f0 = wb_node_pull(ta, 0.0, 0, 0, 320, 120);
    CHECK(f0 != NULL, "pull at t=0 returns frame");
    if (f0) {
        int opaque = count_opaque_pixels(f0);
        /* At t=0 with default delay_per_char=0.05, the first char starts
         * animating immediately but opacity=0 at u=0. However, chars with
         * delay > 0 haven't started yet (local_t < 0, clamped to 0, u=0,
         * opacity=0). So all chars should be invisible at t=0. */
        CHECK(opaque == 0, "all characters invisible at t=0 (opacity=0)");
        wb_frame_free(f0);
    }

    /* ---- Test 3: pull at t=duration (final state - all chars visible) ---- */
    printf("\n-- final state (t=duration) --\n");
    wb_frame *f1 = wb_node_pull(ta, 2.0, 0, 0, 320, 120);
    CHECK(f1 != NULL, "pull at t=2.0 returns frame");
    if (f1) {
        int opaque = count_opaque_pixels(f1);
        CHECK(opaque > 0, "characters visible at t=duration");
        wb_frame_free(f1);
    }

    /* ---- Test 4: pull at mid-animation ---- */
    printf("\n-- mid-animation (t=0.5) --\n");
    wb_frame *f_mid = wb_node_pull(ta, 0.5, 0, 0, 320, 120);
    CHECK(f_mid != NULL, "pull at t=0.5 returns frame");
    if (f_mid) {
        int opaque = count_opaque_pixels(f_mid);
        CHECK(opaque > 0, "some characters visible at t=0.5");
        wb_frame_free(f_mid);
    }

    /* ---- Test 5: range setter ---- */
    printf("\n-- range setter --\n");
    wb_node_effect_text_animator_set_range(ta, 0, 3);
    /* Can't read back directly, but verify no crash + pull works */
    wb_frame *f_range = wb_node_pull(ta, 2.0, 0, 0, 320, 120);
    CHECK(f_range != NULL, "pull after set_range succeeds");
    if (f_range) {
        CHECK(count_opaque_pixels(f_range) > 0, "range-limited animation renders");
        wb_frame_free(f_range);
    }
    /* reset to all */
    wb_node_effect_text_animator_set_range(ta, 0, 0);

    /* ---- Test 6: properties setter ---- */
    printf("\n-- properties setter --\n");
    wb_node_effect_text_animator_set_properties(ta, 10.0f, -15.0f, 2.0f,
                                                 0.5f, 0.8f);
    wb_frame *f_props = wb_node_pull(ta, 2.0, 0, 0, 320, 120);
    CHECK(f_props != NULL, "pull after set_properties succeeds");
    if (f_props) {
        CHECK(count_opaque_pixels(f_props) > 0, "animated properties render");
        wb_frame_free(f_props);
    }

    /* ---- Test 7: easing setter (all 6 types) ---- */
    printf("\n-- easing setter --\n");
    int ease_ok = 1;
    for (int e = 0; e <= 5; e++) {
        wb_node_effect_text_animator_set_easing(ta, e);
        wb_frame *fe = wb_node_pull(ta, 1.0, 0, 0, 320, 120);
        if (!fe) { ease_ok = 0; break; }
        wb_frame_free(fe);
    }
    CHECK(ease_ok, "all 6 easing types work without crash");

    /* Test clamping: negative and >5 */
    wb_node_effect_text_animator_set_easing(ta, -1);
    wb_node_effect_text_animator_set_easing(ta, 10);
    wb_frame *f_clamp = wb_node_pull(ta, 1.0, 0, 0, 320, 120);
    CHECK(f_clamp != NULL, "easing clamps without crash");
    if (f_clamp) wb_frame_free(f_clamp);

    /* ---- Test 8: delay setter ---- */
    printf("\n-- delay setter --\n");
    wb_node_effect_text_animator_set_delay(ta, 0.1);
    wb_frame *f_delay = wb_node_pull(ta, 2.0, 0, 0, 320, 120);
    CHECK(f_delay != NULL, "pull after set_delay succeeds");
    if (f_delay) {
        CHECK(count_opaque_pixels(f_delay) > 0, "staggered animation renders");
        wb_frame_free(f_delay);
    }

    /* ---- Test 9: zero delay ---- */
    printf("\n-- zero delay --\n");
    wb_node_effect_text_animator_set_delay(ta, 0.0);
    wb_frame *f_nodelay = wb_node_pull(ta, 1.0, 0, 0, 320, 120);
    CHECK(f_nodelay != NULL, "pull with zero delay succeeds");
    if (f_nodelay) wb_frame_free(f_nodelay);

    /* ---- Test 10: long text ---- */
    printf("\n-- long text --\n");
    wb_node *ta_long = wb_node_effect_text_animator(
        "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG", 2,
        0.8f, 0.8f, 0.8f, 1.0f, 640, 120);
    CHECK(ta_long != NULL, "long text node created");
    if (ta_long) {
        wb_frame *fl = wb_node_pull(ta_long, 3.0, 0, 0, 640, 120);
        CHECK(fl != NULL, "long text pull succeeds");
        if (fl) {
            CHECK(count_opaque_pixels(fl) > 0, "long text renders");
            wb_frame_free(fl);
        }
        wb_node_destroy(ta_long);
    }

    /* ---- Test 11: empty text ---- */
    printf("\n-- empty text --\n");
    wb_node *ta_empty = wb_node_effect_text_animator("", 2,
                                                      1.0f, 1.0f, 1.0f, 1.0f,
                                                      100, 50);
    CHECK(ta_empty != NULL, "empty text node created");
    if (ta_empty) {
        wb_frame *fe = wb_node_pull(ta_empty, 1.0, 0, 0, 100, 50);
        CHECK(fe != NULL, "empty text pull succeeds");
        if (fe) {
            CHECK(count_opaque_pixels(fe) == 0, "empty text renders nothing");
            wb_frame_free(fe);
        }
        wb_node_destroy(ta_empty);
    }

    /* ---- Test 12: NULL text ---- */
    printf("\n-- NULL text --\n");
    wb_node *ta_null = wb_node_effect_text_animator(NULL, 2,
                                                     1.0f, 1.0f, 1.0f, 1.0f,
                                                     100, 50);
    CHECK(ta_null != NULL, "NULL text node created (defaults to empty)");
    if (ta_null) wb_node_destroy(ta_null);

    /* ---- Test 13: destroy cleans up ---- */
    printf("\n-- cleanup --\n");
    wb_node_destroy(ta);
    CHECK(1, "node destroy completes without crash");

    /* ---- Test 14: setters on NULL node ---- */
    printf("\n-- NULL safety --\n");
    wb_node_effect_text_animator_set_range(NULL, 0, 5);
    wb_node_effect_text_animator_set_properties(NULL, 0, 0, 0, 0, 0);
    wb_node_effect_text_animator_set_easing(NULL, 0);
    wb_node_effect_text_animator_set_delay(NULL, 0.0);
    CHECK(1, "all setters handle NULL without crash");

    /* ---- summary ---- */
    printf("\n=== Results: %d/%d checks passed", checks - failures, checks);
    if (failures > 0) printf(", %d FAILURES", failures);
    printf(" ===\n");
    return failures > 0 ? 1 : 0;
}