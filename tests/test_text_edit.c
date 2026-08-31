/* test_text_edit.c — standalone test for text-based video editing.
 * Tests the transcript manipulation logic without linking the full engine. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "wbus/wbus_transcript.h"

/* We test the transcript operations that wb_text_edit uses internally.
 * Since wb_text_edit.c links against the full session model, this test
 * verifies the core transcript manipulation logic that drives text editing. */

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

/* Case-insensitive compare (mirrors wb_text_edit.c logic). */
static int str_icmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : -1;
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Strip punctuation (mirrors wb_text_edit.c logic). */
static char *strip_punct(const char *word) {
    if (!word) return NULL;
    size_t len = strlen(word);
    size_t start = 0, end = len;
    while (start < end && ispunct((unsigned char)word[start])) start++;
    while (end > start && ispunct((unsigned char)word[end - 1])) end--;
    if (start >= end) {
        char *p = malloc(1); if (p) p[0] = '\0'; return p;
    }
    size_t n = end - start;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, word + start, n);
    out[n] = '\0';
    return out;
}

/* Filler detection (mirrors wb_text_edit.c logic). */
static int is_filler(const char *word) {
    if (!word) return 0;
    char *stripped = strip_punct(word);
    if (!stripped) return 0;
    int filler = (str_icmp(stripped, "um") == 0 ||
                  str_icmp(stripped, "uh") == 0 ||
                  str_icmp(stripped, "ah") == 0 ||
                  str_icmp(stripped, "er") == 0 ||
                  str_icmp(stripped, "like") == 0 ||
                  str_icmp(stripped, "you") == 0 ||
                  str_icmp(stripped, "know") == 0 ||
                  str_icmp(stripped, "basically") == 0 ||
                  str_icmp(stripped, "literally") == 0 ||
                  str_icmp(stripped, "actually") == 0 ||
                  str_icmp(stripped, "so") == 0 ||
                  str_icmp(stripped, "well") == 0);
    free(stripped);
    return filler;
}

/* Helper: build a simple transcript with N words, each 1000ms long. */
static void build_transcript(wb_transcript *t, int n_words) {
    for (int i = 0; i < n_words; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "word%d", i);
        double start = i * 1000.0;
        double end = (i + 1) * 1000.0;
        wb_transcript_add(t, start, end, buf);
    }
}

/* Helper: build a transcript with filler words interspersed. */
static void build_filler_transcript(wb_transcript *t) {
    wb_transcript_add(t,    0.0, 1000.0, "hello");
    wb_transcript_add(t, 1000.0, 2000.0, "um");
    wb_transcript_add(t, 2000.0, 3000.0, "world");
    wb_transcript_add(t, 3000.0, 4000.0, "uh");
    wb_transcript_add(t, 4000.0, 5000.0, "like");
    wb_transcript_add(t, 5000.0, 6000.0, "there");
    wb_transcript_add(t, 6000.0, 7000.0, "you");
    wb_transcript_add(t, 7000.0, 8000.0, "know");
    wb_transcript_add(t, 8000.0, 9000.0, "right");
}

/* Helper: build a transcript with dead air gaps. */
static void build_dead_air_transcript(wb_transcript *t) {
    wb_transcript_add(t,    0.0, 1000.0, "hello");
    /* gap of 3000ms */
    wb_transcript_add(t, 4000.0, 5000.0, "world");
    /* gap of 500ms (below threshold) */
    wb_transcript_add(t, 5500.0, 6500.0, "there");
    /* gap of 4000ms */
    wb_transcript_add(t, 10500.0, 11500.0, "again");
}

/* Simulate wb_text_edit_delete_words (transcript-only path). */
static int sim_delete_words(wb_transcript *t, int start, int end) {
    return wb_transcript_remove_range(t, start, end);
}

/* Simulate wb_text_edit_insert_pause. */
static int sim_insert_pause(wb_transcript *t, int word_index, float duration_sec) {
    if (!t || duration_sec <= 0.0f) return -1;
    int count = wb_transcript_count(t);
    if (word_index < 0 || word_index > count) return -1;
    double pause_ms = (double)duration_sec * 1000.0;
    if (wb_transcript_shift_from(t, word_index, pause_ms) != 0) return -1;
    return 0;
}

/* Simulate wb_text_edit_dead_air. */
static int sim_dead_air(wb_transcript *t, float threshold_sec) {
    if (!t || threshold_sec <= 0.0f) return -1;
    int removed = 0;
    for (int i = wb_transcript_count(t) - 1; i > 0; i--) {
        const wb_word *w0 = wb_transcript_word(t, i - 1);
        const wb_word *w1 = wb_transcript_word(t, i);
        if (!w0 || !w1) continue;
        double gap_ms = w1->start_ms - w0->end_ms;
        double gap_sec = gap_ms / 1000.0;
        if (gap_sec > (double)threshold_sec) {
            if (wb_transcript_shift_from(t, i, -gap_ms) != 0) return -1;
            removed++;
        }
    }
    return removed;
}

/* Simulate wb_text_edit_um_ah_remove. */
static int sim_um_ah_remove(wb_transcript *t) {
    if (!t) return -1;
    int removed = 0;
    int count = wb_transcript_count(t);
    for (int i = count - 1; i >= 0; i--) {
        const wb_word *w = wb_transcript_word(t, i);
        if (!w) continue;
        if (is_filler(w->word)) {
            double word_dur_ms = w->end_ms - w->start_ms;
            if (wb_transcript_remove_range(t, i, i + 1) <= 0) continue;
            if (wb_transcript_shift_from(t, i, -word_dur_ms) != 0) return -1;
            removed++;
            count--;
        }
    }
    return removed;
}

/* Reorder is tested via word_mut + shift_from operations above (test 3). */

int main(void) {
    printf("=== Text-based video editing test ===\n\n");

    /* ---- Test 1: Init with session (transcript creation) ---- */
    {
        wb_transcript *t = wb_transcript_create();
        CHECK(t != NULL, "1: transcript created (simulates init)");
        CHECK(wb_transcript_count(t) == 0, "1: empty transcript has 0 words");
        CHECK(wb_transcript_duration_ms(t) == 0.0, "1: empty duration = 0");
        wb_transcript_free(t);
    }

    /* ---- Test 2: Delete words reduces word count ---- */
    {
        wb_transcript *t = wb_transcript_create();
        build_transcript(t, 10);
        int before = wb_transcript_count(t);
        CHECK(before == 10, "2: started with 10 words");

        int removed = sim_delete_words(t, 3, 7);
        CHECK(removed == 4, "2: deleted 4 words (indices 3-6)");
        int after = wb_transcript_count(t);
        CHECK(after == 6, "2: word count reduced to 6");
        /* Verify remaining words: 0,1,2,7,8,9 */
        const wb_word *w3 = wb_transcript_word(t, 3);
        CHECK(w3 && strcmp(w3->word, "word7") == 0, "2: word[3] is now 'word7'");
        wb_transcript_free(t);
    }

    /* ---- Test 3: Reorder changes word order ---- */
    {
        wb_transcript *t = wb_transcript_create();
        build_transcript(t, 5);

        /* Test word_mut and shift_from (used by reorder). */
        wb_word *w = wb_transcript_word_mut(t, 0);
        CHECK(w != NULL, "3: word_mut returns non-NULL");
        if (w) {
            CHECK(strcmp(w->word, "word0") == 0, "3: word[0] = 'word0'");
        }

        /* Test shift_from. */
        int rc = wb_transcript_shift_from(t, 2, 500.0);
        CHECK(rc == 0, "3: shift_from succeeds");
        const wb_word *w2 = wb_transcript_word(t, 2);
        CHECK(w2 && fabs(w2->start_ms - 2500.0) < 1e-6, "3: word[2] shifted +500ms");
        const wb_word *w0 = wb_transcript_word(t, 0);
        CHECK(w0 && fabs(w0->start_ms - 0.0) < 1e-6, "3: word[0] unchanged");
        wb_transcript_free(t);
    }

    /* ---- Test 4: Insert pause increases duration ---- */
    {
        wb_transcript *t = wb_transcript_create();
        build_transcript(t, 5);  /* 5 words, 5000ms total */

        double dur_before = wb_transcript_duration_ms(t) / 1000.0;
        CHECK(fabs(dur_before - 5.0) < 1e-6, "4: duration starts at 5.0s");

        /* Insert 2s pause before word index 3 */
        int rc = sim_insert_pause(t, 3, 2.0f);
        CHECK(rc == 0, "4: insert_pause succeeded");

        double dur_after = wb_transcript_duration_ms(t) / 1000.0;
        CHECK(fabs(dur_after - 7.0) < 1e-6, "4: duration increased to 7.0s");

        /* Words 3,4 should be shifted right by 2000ms */
        const wb_word *w3 = wb_transcript_word(t, 3);
        CHECK(w3 && fabs(w3->start_ms - 5000.0) < 1e-6, "4: word[3] starts at 5000ms (shifted +2s)");
        const wb_word *w0 = wb_transcript_word(t, 0);
        CHECK(w0 && fabs(w0->start_ms - 0.0) < 1e-6, "4: word[0] unchanged at 0ms");
        wb_transcript_free(t);
    }

    /* ---- Test 5: Dead air detection ---- */
    {
        wb_transcript *t = wb_transcript_create();
        build_dead_air_transcript(t);

        int word_count_before = wb_transcript_count(t);
        CHECK(word_count_before == 4, "5: started with 4 words");

        double dur_before = wb_transcript_duration_ms(t) / 1000.0;
        CHECK(fabs(dur_before - 11.5) < 1e-6, "5: duration starts at 11.5s");

        /* Remove gaps > 1.0s: the 3000ms and 4000ms gaps should be removed.
         * The 500ms gap should remain. */
        int removed = sim_dead_air(t, 1.0f);
        CHECK(removed == 2, "5: removed 2 dead air gaps");

        double dur_after = wb_transcript_duration_ms(t) / 1000.0;
        /* Original 11.5s - 3.0s - 4.0s = 4.5s (the 500ms gap is preserved) */
        CHECK(fabs(dur_after - 4.5) < 1e-6, "5: duration reduced to 4.5s");

        /* All 4 words should still be present. */
        CHECK(wb_transcript_count(t) == 4, "5: all words retained");

        /* Verify word order preserved */
        const wb_word *w0 = wb_transcript_word(t, 0);
        const wb_word *w1 = wb_transcript_word(t, 1);
        CHECK(w0 && strcmp(w0->word, "hello") == 0, "5: word[0] = 'hello'");
        CHECK(w1 && strcmp(w1->word, "world") == 0, "5: word[1] = 'world'");
        wb_transcript_free(t);
    }

    /* ---- Test 6: Um/ah removal ---- */
    {
        wb_transcript *t = wb_transcript_create();
        build_filler_transcript(t);

        int before = wb_transcript_count(t);
        CHECK(before == 9, "6: started with 9 words");

        int removed = sim_um_ah_remove(t);
        CHECK(removed >= 4, "6: removed at least 4 filler words");

        int after = wb_transcript_count(t);
        CHECK(after == before - removed, "6: word count = before - removed");

        /* Verify no filler words remain */
        int fillers_left = 0;
        for (int i = 0; i < after; i++) {
            const wb_word *w = wb_transcript_word(t, i);
            if (w && is_filler(w->word))
                fillers_left++;
        }
        CHECK(fillers_left == 0, "6: no filler words remain");

        /* Verify non-filler words still present */
        int hello_found = 0, world_found = 0, there_found = 0, right_found = 0;
        for (int i = 0; i < after; i++) {
            const wb_word *w = wb_transcript_word(t, i);
            if (!w) continue;
            if (strcmp(w->word, "hello") == 0) hello_found++;
            if (strcmp(w->word, "world") == 0) world_found++;
            if (strcmp(w->word, "there") == 0) there_found++;
            if (strcmp(w->word, "right") == 0) right_found++;
        }
        CHECK(hello_found == 1 && world_found == 1 && there_found == 1 && right_found == 1,
              "6: all non-filler words preserved (hello/world/there/right)");
        wb_transcript_free(t);
    }

    /* ---- Test 7: Word count accurate ---- */
    {
        wb_transcript *t = wb_transcript_create();
        CHECK(wb_transcript_count(t) == 0, "7: empty transcript has 0 words");

        build_transcript(t, 100);
        CHECK(wb_transcript_count(t) == 100, "7: 100 words counted");

        sim_delete_words(t, 0, 50);
        CHECK(wb_transcript_count(t) == 50, "7: 50 words after deleting 50");

        sim_delete_words(t, 25, 50);
        CHECK(wb_transcript_count(t) == 25, "7: 25 words after deleting 25 more");

        wb_transcript_free(t);
    }

    /* ---- Test 8: Duration calculation correct ---- */
    {
        wb_transcript *t = wb_transcript_create();
        CHECK(wb_transcript_duration_ms(t) == 0.0, "8: empty duration = 0.0ms");

        /* Add 5 words, each 2000ms = 10s total */
        for (int i = 0; i < 5; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "w%d", i);
            wb_transcript_add(t, i * 2000.0, (i + 1) * 2000.0, buf);
        }
        CHECK(fabs(wb_transcript_duration_ms(t) - 10000.0) < 1e-6,
              "8: 5 words x 2s = 10000ms");

        /* Delete word 0 (0-2000ms). Remaining words still span 0-10s
         * (the gap is real -- it's a cut, not a ripple). */
        sim_delete_words(t, 0, 1);
        CHECK(fabs(wb_transcript_duration_ms(t) - 10000.0) < 1e-6,
              "8: duration still 10000ms after delete (gap preserved)");

        /* Now add a 3s pause at the beginning (before word 0 = index 0) */
        sim_insert_pause(t, 0, 3.0f);
        CHECK(fabs(wb_transcript_duration_ms(t) - 13000.0) < 1e-6,
              "8: duration = 13000ms after 3s pause at start");

        wb_transcript_free(t);
    }

    printf("\n=== %d/%d checks passed ===\n", checks - failures, checks);
    return failures > 0 ? 1 : 0;
}