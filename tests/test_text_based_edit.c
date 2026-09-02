/* test_text_based_edit.c — test text-based video editing (R086) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_edit.h"
#include "wbus/wbus_compositor.h"
#include "wbus/wb_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Declarations for text-based edit functions */
int wb_edit_transcript_to_edits(wb_edit_graph *g, const char *transcript_json,
                                 const char *edit_rules);
int wb_edit_search_transcript(wb_edit_graph *g, const char *query);
int wb_edit_delete_silence(wb_edit_graph *g, float threshold_db,
                            float min_duration);
int wb_edit_text_undo(wb_edit_graph *g);

#ifdef __cplusplus
}
#endif

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); pass = 0; } \
    else { printf("ok: %s\n", msg); } \
} while(0)

int main(void) {
    int pass = 1;

    printf("-- text-based editing --\n");

    /* Test 1: NULL graph handling */
    {
        int rc = wb_edit_transcript_to_edits(NULL, "[]", NULL);
        CHECK(rc == -1, "transcript_to_edits with NULL graph returns -1");

        rc = wb_edit_delete_silence(NULL, -40.0f, 0.5f);
        CHECK(rc == -1, "delete_silence with NULL graph returns -1");

        int matches = wb_edit_search_transcript(NULL, "test");
        CHECK(matches == -1, "search_transcript with NULL graph returns -1");

        rc = wb_edit_text_undo(NULL);
        CHECK(rc == -1, "text_undo with NULL graph returns -1");
    }

    /* Test 2: Create edit graph and test with empty/edge-case transcripts */
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        CHECK(g != NULL, "edit graph created for text-based tests");

        /* Empty transcript array */
        int rc = wb_edit_transcript_to_edits(g, "[]", NULL);
        CHECK(rc == -1, "transcript_to_edits with empty array returns -1");

        /* Invalid JSON */
        rc = wb_edit_transcript_to_edits(g, "not json", NULL);
        CHECK(rc == -1, "transcript_to_edits with invalid JSON returns -1");

        /* NULL transcript */
        rc = wb_edit_transcript_to_edits(g, NULL, NULL);
        CHECK(rc == -1, "transcript_to_edits with NULL transcript returns -1");

        /* Valid transcript but no clips to edit */
        const char *transcript = "[{\"start\":0.0,\"end\":1.0,\"text\":\"hello world\"}]";
        rc = wb_edit_transcript_to_edits(g, transcript, NULL);
        CHECK(rc == 0, "transcript_to_edits with valid JSON but no clips returns 0");

        /* Search with no clips */
        int matches = wb_edit_search_transcript(g, "hello");
        CHECK(matches == 0, "search_transcript with no clips returns 0");

        /* Silence detection with no audio clips */
        rc = wb_edit_delete_silence(g, -40.0f, 0.5f);
        CHECK(rc == 0, "delete_silence with no audio clips returns 0");

        /* Undo with no text edits */
        rc = wb_edit_text_undo(g);
        CHECK(rc == -1, "text_undo with no edits returns -1");

        wb_edit_graph_destroy(g);
    }

    /* Test 3: Transcript with clips on track */
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        CHECK(g != NULL, "edit graph created for transcript+clip test");

        int t = wb_edit_add_track(g, "V1");
        CHECK(t == 0, "track added for transcript test");

        /* We can't add real video clips without files, but we can test
         * the API contract: transcript with clips present but no overlapping */
        const char *transcript = "["
            "{\"start\":0.0,\"end\":2.0,\"text\":\"hello world\"},"
            "{\"start\":2.0,\"end\":4.0,\"text\":\"foo bar baz\"}"
        "]";

        /* No clips overlap, so 0 edits */
        int rc = wb_edit_transcript_to_edits(g, transcript, NULL);
        CHECK(rc == 0, "transcript with no overlapping clips returns 0 edits");

        /* Test with remove rules */
        rc = wb_edit_transcript_to_edits(g, transcript, "remove=hello,foo");
        CHECK(rc == 0, "transcript with remove rules but no clips returns 0");

        /* Test with keep-all rules */
        rc = wb_edit_transcript_to_edits(g, transcript, "keep-all");
        CHECK(rc == 0, "transcript with keep-all rules but no clips returns 0");

        wb_edit_graph_destroy(g);
    }

    /* Test 4: Search with clips */
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        CHECK(g != NULL, "edit graph created for search test");

        int t = wb_edit_add_track(g, "V1");
        CHECK(t == 0, "track added for search test");

        /* Search with empty query */
        int matches = wb_edit_search_transcript(g, "");
        CHECK(matches == -1, "search with empty query returns -1");

        /* Search with NULL query */
        matches = wb_edit_search_transcript(g, NULL);
        CHECK(matches == -1, "search with NULL query returns -1");

        /* Search for time value with no clips */
        matches = wb_edit_search_transcript(g, "0.0");
        CHECK(matches == 0, "search for time with no clips returns 0");

        wb_edit_graph_destroy(g);
    }

    /* Test 5: Silence detection edge cases */
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        CHECK(g != NULL, "edit graph created for silence test");

        int t = wb_edit_add_track(g, "V1");
        CHECK(t == 0, "track added for silence test");

        /* Test with default threshold */
        int rc = wb_edit_delete_silence(g, -40.0f, 0.5f);
        CHECK(rc == 0, "delete_silence with defaults returns 0 (no audio clips)");

        /* Test with positive threshold (should be clamped) */
        rc = wb_edit_delete_silence(g, 10.0f, 0.5f);
        CHECK(rc == 0, "delete_silence with positive threshold clamped");

        /* Test with zero min_duration (should be clamped) */
        rc = wb_edit_delete_silence(g, -40.0f, 0.0f);
        CHECK(rc == 0, "delete_silence with zero min_duration clamped");

        wb_edit_graph_destroy(g);
    }

    /* Test 6: Transcript JSON parsing edge cases */
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        CHECK(g != NULL, "edit graph created for JSON parsing test");

        int t = wb_edit_add_track(g, "V1");
        CHECK(t == 0, "track added for JSON parsing test");

        /* Transcript with special characters in text */
        const char *transcript = "["
            "{\"start\":0.0,\"end\":1.0,\"text\":\"hello\\nworld\"},"
            "{\"start\":1.0,\"end\":2.0,\"text\":\"tab\\there\"},"
            "{\"start\":2.0,\"end\":3.0,\"text\":\"quote\\\"test\\\"\"}"
        "]";
        int rc = wb_edit_transcript_to_edits(g, transcript, NULL);
        CHECK(rc >= 0, "transcript with escaped chars parsed successfully");

        /* Transcript with single word */
        transcript = "[{\"start\":0.0,\"end\":1.0,\"text\":\"single\"}]";
        rc = wb_edit_transcript_to_edits(g, transcript, NULL);
        CHECK(rc >= 0, "transcript with single word parsed successfully");

        /* Transcript with many words */
        transcript = "[{\"start\":0.0,\"end\":10.0,\"text\":\"one two three four five six seven eight nine ten\"}]";
        rc = wb_edit_transcript_to_edits(g, transcript, NULL);
        CHECK(rc >= 0, "transcript with many words parsed successfully");

        wb_edit_graph_destroy(g);
    }

    /* Test 7: Multiple tracks */
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        CHECK(g != NULL, "edit graph created for multi-track test");

        int t0 = wb_edit_add_track(g, "V1");
        int t1 = wb_edit_add_track(g, "V2");
        CHECK(t0 == 0 && t1 == 1, "two tracks added for multi-track test");

        const char *transcript = "[{\"start\":0.0,\"end\":5.0,\"text\":\"test clip editing\"}]";
        int rc = wb_edit_transcript_to_edits(g, transcript, NULL);
        CHECK(rc >= 0, "transcript_to_edits with multiple tracks works");

        int matches = wb_edit_search_transcript(g, "test");
        CHECK(matches >= 0, "search with multiple tracks works");

        wb_edit_graph_destroy(g);
    }

    printf("\n-- text-based editing tests %s --\n", pass ? "PASSED" : "FAILED");
    return pass ? 0 : 1;
}