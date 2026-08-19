/* test_captions.c — headless test for the auto-captions pipeline.
 *
 * Verifies: audio extraction, whisper transcription, SRT write, transcript read.
 * Uses the JFK PCM as a known-good test source (already verified by wb_whisper_test).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus_captions.h"

static int failures = 0;
static int checks = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

int main(void) {
    printf("=== Auto-captions pipeline test ===\n\n");

    /* 1. Create captions context */
    wb_captions *cap = wb_captions_create();
    CHECK(cap != NULL, "captions context created");

    /* 2. Generate captions from the JFK audio file (already verified PCM).
     * First, convert the PCM to a WAV that ffmpeg can read. */
    system("/Users/waefrebeorn/homebrew/bin/ffmpeg -y -f s16le -ar 16000 -ac 1 -i /tmp/jfk.pcm -c:a pcm_s16le /tmp/jfk_captions_test.wav > /dev/null 2>&1");

    const char *srt = NULL;
    const char *txt = NULL;
    int rc = wb_captions_generate(cap, "/tmp/jfk_captions_test.wav");
    CHECK(rc == 0, "captions generated from JFK audio");

    /* 3. Check transcript */
    const char *transcript = wb_captions_get_transcript(cap);
    CHECK(transcript != NULL, "transcript text available");
    if (transcript) {
        printf("         transcript: \"%s\"\n", transcript);
        CHECK(strstr(transcript, "country") != NULL ||
              strstr(transcript, "ask not") != NULL,
              "transcript contains expected JFK content");
    }

    /* 4. Write SRT */
    char srt_path[512];
    snprintf(srt_path, sizeof(srt_path), "%s/test_captions.srt", "/tmp");
    rc = wb_captions_write_srt(srt_path, transcript ? transcript : "test", 106000);
    CHECK(rc == 0, "SRT written to disk");
    FILE *f = fopen(srt_path, "r");
    CHECK(f != NULL, "SRT file exists on disk");
    if (f) {
        char buf[256];
        size_t n = fread(buf, 1, sizeof(buf)-1, f);
        buf[n] = '\0';
        fclose(f);
        CHECK(strstr(buf, "1") != NULL, "SRT contains sequence number");
        CHECK(strstr(buf, "-->") != NULL, "SRT contains timecode arrow");
        remove(srt_path);
    }

    /* 5. Cleanup */
    wb_captions_cleanup(cap);
    wb_captions_free(cap);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
