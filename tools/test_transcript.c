/* test_transcript.c — headless verification of the editable transcript
 * model (R015 G6): SRT parse -> word-level timing -> click-to-seek
 * (word_at) -> drag-range select -> SRT round-trip. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_transcript.h"
#include "wbus.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

/* Deterministic SRT sample (2 cues, 3 + 2 words). */
static const char *SAMPLE =
    "1\n"
    "00:00:00,000 --> 00:00:02,000\n"
    "the quick brown\n"
    "\n"
    "2\n"
    "00:00:02,000 --> 00:00:05,000\n"
    "fox jumps\n";

int main(void) {
    printf("=== Transcript (G6) test ===\n\n");

    /* write sample SRT to a temp file */
    const char *path = "/tmp/bigmac_transcript_test.srt";
    FILE *f = fopen(path, "w");
    if (!f) { printf("cannot write temp srt\n"); return 2; }
    fputs(SAMPLE, f);
    fclose(f);

    wb_transcript *t = wb_transcript_from_srt(path);
    CHECK(t != NULL, "parsed SRT into transcript");
    if (!t) return 1;

    CHECK(wb_transcript_count(t) == 5, "5 words (3 + 2) parsed");
    printf("         count=%d duration=%.0f ms\n",
           wb_transcript_count(t), wb_transcript_duration_ms(t));
    CHECK(wb_transcript_duration_ms(t) == 5000.0, "duration = last word end (5000ms)");

    /* verify first/last word text */
    const wb_word *w0 = wb_transcript_word(t, 0);
    const wb_word *w4 = wb_transcript_word(t, 4);
    CHECK(w0 && strcmp(w0->word, "the") == 0, "word[0] = 'the'");
    CHECK(w4 && strcmp(w4->word, "jumps") == 0, "word[4] = 'jumps'");

    /* word-level timing: cue1 [0,2000] over 3 words => each ~666ms */
    CHECK(fabs(w0->start_ms - 0.0) < 1e-6, "word0 start=0");
    CHECK(fabs(w0->end_ms - 666.67) < 1.0, "word0 end~666ms");
    const wb_word *w2 = wb_transcript_word(t, 2); /* 'brown', 3rd of cue1 */
    CHECK(fabs(w2->end_ms - 2000.0) < 1.0, "word2 end=2000ms (cue1 boundary)");
    const wb_word *w3 = wb_transcript_word(t, 3); /* 'fox', cue2 start */
    CHECK(fabs(w3->start_ms - 2000.0) < 1.0, "word3 start=2000ms (cue2 boundary)");
    CHECK(fabs(w3->end_ms - 3500.0) < 1.0, "word3 end~3500ms (cue2 2 words, 1st half)");

    /* click-to-seek: word whose span contains 1000ms */
    int at = wb_transcript_word_at(t, 1000.0);
    CHECK(at >= 0 && at < 5, "word_at(1000ms) in range");
    if (at >= 0) {
        const wb_word *wf = wb_transcript_word(t, at);
        CHECK(1000.0 >= wf->start_ms && 1000.0 < wf->end_ms,
              "word_at returns the word spanning 1000ms (click-to-seek)");
    }

    /* drag-range select: words overlapping [500, 2500] */
    int sel_start = wb_transcript_word_at(t, 500.0);
    int sel_end = wb_transcript_word_at(t, 2500.0);
    CHECK(sel_start >= 0 && sel_end > sel_start, "drag selects a word range");
    printf("         selection words [%d..%d] = '%.*s'..'%.*s'\n",
           sel_start, sel_end,
           (int)strlen(wb_transcript_word(t, sel_start)->word),
           wb_transcript_word(t, sel_start)->word,
           (int)strlen(wb_transcript_word(t, sel_end)->word),
           wb_transcript_word(t, sel_end)->word);

    /* edit a word (keep timing) */
    wb_transcript_set_word(t, 1, "slow");
    CHECK(strcmp(wb_transcript_word(t, 1)->word, "slow") == 0, "word edit keeps slot");

    /* SRT round-trip */
    const char *out = "/tmp/bigmac_transcript_out.srt";
    CHECK(wb_transcript_write_srt(t, out) == 0, "wrote round-trip SRT");
    wb_transcript *t2 = wb_transcript_from_srt(out);
    CHECK(t2 && wb_transcript_count(t2) == 5, "re-parsed round-trip has 5 words");
    if (t2) {
        CHECK(strcmp(wb_transcript_word(t2, 1)->word, "slow") == 0,
              "edited word survived round-trip");
        CHECK(fabs(wb_transcript_duration_ms(t2) - 5000.0) < 1.0,
              "duration survived round-trip");
        wb_transcript_free(t2);
    }

    /* ---- R048: word-range delete (Descript text editing) ---- */
    wb_transcript *t3 = wb_transcript_from_srt(path);   /* fresh: 5 words */
    CHECK(t3 && wb_transcript_count(t3) == 5, "fresh transcript for delete test");
    int nrem = wb_transcript_remove_range(t3, 1, 3);    /* drop 'quick','brown' */
    CHECK(nrem == 2, "remove_range removed 2 words");
    CHECK(wb_transcript_count(t3) == 3, "3 words remain");
    CHECK(strcmp(wb_transcript_word(t3, 0)->word, "the") == 0 &&
          strcmp(wb_transcript_word(t3, 1)->word, "fox") == 0,
          "remaining words are 'the','fox','jumps' (order kept)");
    const wb_word *rj = wb_transcript_word(t3, 2);
    CHECK(rj && strcmp(rj->word, "jumps") == 0 &&
          fabs(rj->end_ms - 5000.0) < 1e-6,
          "kept word keeps its ORIGINAL timing (gap is real)");
    CHECK(wb_transcript_remove_range(t3, 2, 5) == -1, "bad range rejected");
    CHECK(wb_transcript_remove_range(NULL, 0, 1) == -1, "NULL rejected");
    wb_transcript_free(t3);

    /* ---- R048: session-level transcript cut (words -> media ripple cut) -- */
    {
        wb_session *s = wb_session_create();
        s->length = 44100.0 * 10.0;
        int vt = wb_session_add_video_track(s, "V");
        /* two clips on the video track: [0,3)s and [3,6)s of a fake source */
        wb_session_add_video_clip(s, vt, "/tmp/fake_src.mp4", 0.0);
        wb_session_add_video_clip(s, vt, "/tmp/fake_src.mp4", 3.0);
        wb_track *vtr = &s->tracks[vt];
        vtr->clips[0].length = 3.0;
        vtr->clips[1].length = 3.0;
        CHECK(vtr->clip_count == 2, "two video clips laid down");

        wb_transcript *tt = wb_transcript_create();
        /* words spanning the timeline: w1..w2 live in [1.0, 2.5)s — fully
         * inside clip 0's span; cutting them must split + shrink it. */
        wb_transcript_add(tt, 0.0,    1000.0, "alpha");
        wb_transcript_add(tt, 1000.0, 1750.0, "beta");
        wb_transcript_add(tt, 1750.0, 2500.0, "gamma");
        wb_transcript_add(tt, 2500.0, 4000.0, "delta");

        int rc = wb_session_transcript_cut(s, vt, tt, 1, 3);
        CHECK(rc == 0, "transcript_cut succeeded");
        CHECK(wb_transcript_count(tt) == 2, "transcript now 2 words");
        CHECK(strcmp(wb_transcript_word(tt, 1)->word, "delta") == 0,
              "'delta' remains after cut");
        printf("         clips after cut=%u\n", vtr->clip_count);
        for (uint32_t c = 0; c < vtr->clip_count; c++)
            printf("         clip[%u] start=%.2f len=%.2f\n",
                   c, vtr->clips[c].start, vtr->clips[c].length);
        /* total media must have shrunk by exactly 1.5s (2500-1000 ms) */
        double total = 0;
        for (uint32_t c = 0; c < vtr->clip_count; c++) total += vtr->clips[c].length;
        CHECK(fabs(total - 4.5) < 0.01, "total media shrank by exactly 1.5s");
        /* no clip may still overlap the deleted span [1.0,2.5) at its old
         * position — verify contiguity instead: starts must be monotonic
         * and adjacent (no gap where the cut happened). */
        for (uint32_t c = 1; c < vtr->clip_count; c++)
            CHECK(fabs(vtr->clips[c].start -
                       (vtr->clips[c-1].start + vtr->clips[c-1].length)) < 0.02,
                  "clips remain contiguous after ripple (no hole)");
        wb_transcript_free(tt);
        wb_session_destroy(s);
    }

    wb_transcript_free(t);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
