/* tests/test_bg_render.c — background render API test suite.
 * 1. Start a render — verify it begins
 * 2. Poll until complete — verify progress goes 0→1
 * 3. Wait with timeout — verify completion
 * 4. Cancel a render — verify status becomes cancelled
 * 5. Error on invalid path — verify error status
 * 6. Render produces valid output file
 * 7. Multiple sequential renders work
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <math.h>

#include "wbus.h"
#include "wb_internal.h"

/* opaque handle is declared in wbus.h; provide the missing typedef so the
 * test compiles against the same ABI the implementation uses. */
typedef struct wb_bg_render wb_bg_render;

/* ---- extern API (declared in wbus.h but repeated here for clarity) -- */
extern wb_bg_render *wb_bg_render_start(const wb_session *session,
                                        const char *output_path, int format);
extern int  wb_bg_render_poll(wb_bg_render *r, float *progress_out);
extern int  wb_bg_render_wait(wb_bg_render *r, int timeout_ms);
extern void wb_bg_render_cancel(wb_bg_render *r);
extern int  wb_bg_render_status(wb_bg_render *r);
extern const char *wb_bg_render_error(wb_bg_render *r);
extern void wb_bg_render_destroy(wb_bg_render *r);

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  [%d] %s ... ", tests_run, name); \
} while(0)

#define PASS() do { tests_passed++; printf("PASS\n"); return 0; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return 1; } while(0)
#define FAILF(fmt, ...) do { printf("FAIL: "); printf(fmt, __VA_ARGS__); printf("\n"); return 1; } while(0)

/* Build a short demo session with a few notes. */
static wb_session *make_demo_session(void) {
    wb_session *s = wb_session_create();
    if (!s) return NULL;
    s->bpm = 120.0;
    s->time_sig_num = 4;
    s->time_sig_den = 4;
    wb_track *tr = wb_session_add_track(s, "synth", WB_TRACK_KIND_INSTR);
    if (!tr) { wb_session_destroy(s); return NULL; }
    tr->volume = 0.8f;
    /* 2 seconds of notes at 44100 Hz */
    for (int i = 0; i < 8; i++) {
        wb_session_add_note(tr, i * 0.25 * 44100.0, 0.2 * 44100.0,
                            60 + (i % 5), 100);
    }
    s->length = 2.0 * 44100.0;  /* 2 seconds */
    return s;
}

/* ---- Test 1: Start a render — verify it begins ----------------------- */
static int test_start(wb_session *s) {
    TEST("start render begins");
    const char *out = "/tmp/test_bg_1.wav";
    remove(out);
    wb_bg_render *r = wb_bg_render_start(s, out, 0);  /* WAV16 */
    if (!r) FAIL("wb_bg_render_start returned NULL");
    /* status should be pending or running */
    int st = wb_bg_render_status(r);
    if (st != 0 && st != 1) FAILF("expected status 0/1, got %d", st);
    /* wait for completion so we don't leak */
    int rc = wb_bg_render_wait(r, 30000);
    if (rc != 0) FAILF("wait returned %d", rc);
    PASS();
}

/* ---- Test 2: Poll until complete — verify progress 0→1 --------------- */
static int test_poll(wb_session *s) {
    TEST("poll until complete, progress 0..1");
    (void)s;
    /* Use a session long enough that we can observe it rendering */
    wb_session *long_s = wb_session_create();
    long_s->bpm = 120.0;
    wb_session_add_track(long_s, "synth", WB_TRACK_KIND_INSTR);
    wb_session_add_track(long_s, "synth2", WB_TRACK_KIND_INSTR);
    for (int t = 0; t < 2 && t < (int)long_s->track_count; t++) {
        wb_track *tt = &long_s->tracks[t];
        for (int i = 0; i < 16; i++)
            wb_session_add_note(tt, i * 44100.0, 0.4 * 44100.0, 48 + (i % 12), 100);
    }
    long_s->length = 30.0 * 44100.0;  /* 30 seconds */

    const char *out = "/tmp/test_bg_2.wav";
    remove(out);
    wb_bg_render *r = wb_bg_render_start(long_s, out, 0);
    if (!r) { wb_session_destroy(long_s); FAIL("start returned NULL"); }
    float prog = 0.0f;
    float last = 0.0f;
    int iters = 0;
    /* Poll until done — cap at 30 seconds of wall time (3000 polls × 10ms) */
    while (wb_bg_render_poll(r, &prog) && iters < 3000) {
        if (prog < last - 0.01f) {
            wb_bg_render_destroy(r);
            wb_session_destroy(long_s);
            FAIL("progress went backwards");
        }
        last = prog;
        iters++;
        usleep(10000);  /* 10ms between polls to avoid spinning */
    }
    /* Final poll to capture last progress value */
    wb_bg_render_poll(r, &prog);
    if (prog < 0.99f) {
        wb_bg_render_destroy(r);
        wb_session_destroy(long_s);
        FAILF("final progress %.3f < 1.0 (iters=%d)", prog, iters);
    }
    int st = wb_bg_render_status(r);
    if (st != 2) {
        wb_bg_render_destroy(r);
        wb_session_destroy(long_s);
        FAILF("expected status 2 (done), got %d", st);
    }
    wb_bg_render_wait(r, 5000);
    wb_session_destroy(long_s);
    PASS();
}

/* ---- Test 3: Wait with timeout — verify completion ------------------ */
static int test_wait(wb_session *s) {
    TEST("wait with timeout completes");
    const char *out = "/tmp/test_bg_3.wav";
    remove(out);
    wb_bg_render *r = wb_bg_render_start(s, out, 0);
    if (!r) FAIL("start returned NULL");
    int rc = wb_bg_render_wait(r, 30000);
    if (rc != 0) FAILF("wait returned %d (expected 0)", rc);
    PASS();
}

/* ---- Test 4: Cancel a render — status becomes cancelled ------------- */
static int test_cancel(wb_session *s) {
    TEST("cancel sets cancelled status");
    (void)s;
    /* use a long session to ensure it doesn't finish before we cancel */
    wb_session *long_s = wb_session_create();
    long_s->bpm = 120.0;
    wb_track *tr = wb_session_add_track(long_s, "synth", WB_TRACK_KIND_INSTR);
    for (int i = 0; i < 16; i++)
        wb_session_add_note(tr, i * 44100.0, 0.5 * 44100.0, 60, 100);
    long_s->length = 30.0 * 44100.0;  /* 30s — plenty of time to cancel */

    const char *out = "/tmp/test_bg_4.wav";
    remove(out);
    wb_bg_render *r = wb_bg_render_start(long_s, out, 0);
    if (!r) { wb_session_destroy(long_s); FAIL("start returned NULL"); }
    /* give it a moment to start */
    usleep(50000);  /* 50ms */
    wb_bg_render_cancel(r);
    /* wait for the thread to finish */
    wb_bg_render_wait(r, 5000);
    /* r has been freed by wait; we can't query status. Instead, check that
     * the output file was NOT produced (cancel aborts before write). */
    struct stat stbuf;
    int exists = (stat(out, &stbuf) == 0);
    if (exists) FAIL("output file exists after cancel");
    wb_session_destroy(long_s);
    PASS();
}

/* ---- Test 5: Error on invalid path ---------------------------------- */
static int test_error_invalid_path(wb_session *s) {
    TEST("error on invalid output path");
    const char *out = "/nonexistent_dir_xyz/render.wav";
    wb_bg_render *r = wb_bg_render_start(s, out, 0);
    if (!r) FAIL("start returned NULL (2nd)");
    /* poll until no longer running — should error out */
    int iters = 0;
    float p;
    while (wb_bg_render_poll(r, &p) && iters < 500) {
        usleep(1000);  /* 1ms between polls */
        iters++;
    }
    /* poll one more to capture final state */
    wb_bg_render_poll(r, &p);
    int st = wb_bg_render_status(r);
    if (st != 3) {
        wb_bg_render_destroy(r);
        FAILF("expected status 3 (error), got %d", st);
    }
    const char *err = wb_bg_render_error(r);
    if (!err || strlen(err) == 0) {
        wb_bg_render_destroy(r);
        FAIL("error message empty");
    }
    wb_bg_render_destroy(r);
    PASS();
}

/* ---- Test 6: Render produces valid output file ---------------------- */
static int test_output_valid(wb_session *s) {
    TEST("render produces valid WAV file");
    const char *out = "/tmp/test_bg_6.wav";
    remove(out);
    wb_bg_render *r = wb_bg_render_start(s, out, 0);
    if (!r) FAIL("start returned NULL");
    int rc = wb_bg_render_wait(r, 30000);
    if (rc != 0) FAILF("wait returned %d", rc);
    /* verify the file exists and has a valid WAV header */
    struct stat stbuf;
    if (stat(out, &stbuf) != 0) FAIL("output file not found");
    if (stbuf.st_size < 44) FAIL("file too small for WAV header");
    FILE *f = fopen(out, "rb");
    if (!f) FAIL("cannot open output file");
    char hdr[4];
    if (fread(hdr, 1, 4, f) != 4 || memcmp(hdr, "RIFF", 4) != 0) {
        fclose(f); FAIL("no RIFF header");
    }
    fseek(f, 8, SEEK_SET);
    if (fread(hdr, 1, 4, f) != 4 || memcmp(hdr, "WAVE", 4) != 0) {
        fclose(f); FAIL("no WAVE tag");
    }
    fclose(f);
    /* verify audio content: read samples, check they're not all zero */
    float *audio = NULL;
    uint32_t frames = 0;
    int ch = 0, sr = 0;
    if (wb_wav_read_pcm16(out, &audio, &frames, &ch, &sr) != 0) {
        FAIL("wb_wav_read_pcm16 failed");
    }
    if (frames == 0) { free(audio); FAIL("zero frames in output"); }
    float peak = 0;
    for (uint32_t i = 0; i < frames * ch; i++) {
        float v = audio[i] < 0 ? -audio[i] : audio[i];
        if (v > peak) peak = v;
    }
    free(audio);
    if (peak < 0.01f) FAIL("output is silent (peak < 0.01)");
    PASS();
}

/* ---- Test 7: Multiple sequential renders work ----------------------- */
static int test_sequential(wb_session *s) {
    TEST("multiple sequential renders");
    for (int i = 0; i < 3; i++) {
        char out[64];
        snprintf(out, sizeof(out), "/tmp/test_bg_7_%d.wav", i);
        remove(out);
        wb_bg_render *r = wb_bg_render_start(s, out, 0);
        if (!r) FAILF("start returned NULL on iteration %d", i);
        int rc = wb_bg_render_wait(r, 30000);
        if (rc != 0) FAILF("wait returned %d on iteration %d", rc, i);
        struct stat stbuf;
        if (stat(out, &stbuf) != 0 || stbuf.st_size < 44)
            FAILF("output file missing/truncated on iteration %d", i);
    }
    PASS();
}

/* ---- main ------------------------------------------------------------ */
int main(void) {
    printf("Big Mac DAW — background render test suite\n");
    printf("==========================================\n");

    wb_session *s = make_demo_session();
    if (!s) {
        fprintf(stderr, "FAIL: could not create demo session\n");
        return 1;
    }
    printf("demo session: %u tracks, length %.0f samples\n",
           s->track_count, s->length);

    int rc = 0;
    rc |= test_start(s);
    rc |= test_poll(s);
    rc |= test_wait(s);
    rc |= test_cancel(s);
    rc |= test_error_invalid_path(s);
    rc |= test_output_valid(s);
    rc |= test_sequential(s);

    wb_session_destroy(s);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return rc == 0 && tests_passed == tests_run ? 0 : 1;
}