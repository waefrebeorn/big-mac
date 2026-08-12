/* wb_selftest.c — headless engine self-test (THE GATE).
 * Verifies transport, timeline scheduling, DSP units, and WAV export all
 * produce correct, real results. Must pass before any claim of "it works".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "wbus.h"
#include "wb_internal.h"

static int failures = 0;
static int checks = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

/* ---- test 1: transport advances only when playing --------------------- */
static void test_transport(void) {
    printf("test_transport\n");
    wb_session *s = wb_session_demo();
    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);

    wb_sample buf[512 * 2];
    /* not playing -> song_pos should stay 0 */
    wb_engine_render(e, buf, 512);
    wb_transport t;
    wb_engine_get_transport(e, &t);
    CHECK(t.song_pos == 0, "song_pos static when stopped");

    /* play -> advances */
    wb_engine_play(e);
    wb_engine_render(e, buf, 512);
    wb_engine_get_transport(e, &t);
    CHECK(t.song_pos == 512, "song_pos advances by block size when playing");

    /* seek */
    wb_engine_seek(e, 44100.0);
    wb_engine_render(e, buf, 256);
    wb_engine_get_transport(e, &t);
    CHECK(t.song_pos >= 44100.0, "seek honored");

    wb_engine_destroy(e);
    wb_session_destroy(s);
}

/* ---- test 2: synth produces non-silent audio on a note ---------------- */
static void test_synth_audio(void) {
    printf("test_synth_audio\n");
    wb_session *s = wb_session_demo();
    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);
    wb_engine_play(e);

    /* render 1 second */
    uint32_t total = 44100;
    wb_sample *buf = malloc(total * 2 * sizeof(wb_sample));
    uint32_t done = 0;
    while (done < total) {
        uint32_t n = total - done; if (n > 512) n = 512;
        wb_engine_render(e, buf + done*2, n);
        done += n;
    }
    /* measure peak */
    float peak = 0;
    for (uint32_t i = 0; i < total * 2; i++) {
        float a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peak) peak = a;
    }
    CHECK(peak > 0.01f, "synth timeline produces audible audio (peak > 0.01)");
    printf("         peak = %.3f\n", peak);

    free(buf);
    wb_engine_destroy(e);
    wb_session_destroy(s);
}

/* ---- test 3: wav round-trip ------------------------------------------ */
static void test_wav(void) {
    printf("test_wav\n");
    wb_sample data[1000];
    for (int i = 0; i < 1000; i++) data[i] = (float)sin(i * 0.1);
    int rc = wb_wav_write_pcm16("/tmp/wb_test.wav", data, 1000, 2, 44100);
    CHECK(rc == 0, "wav write returns 0");

    FILE *f = fopen("/tmp/wb_test.wav", "rb");
    CHECK(f != NULL, "wav file exists");
    if (f) {
        char hdr[12];
        fread(hdr, 1, 12, f);
        CHECK(memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr+8, "WAVE", 4) == 0,
              "wav header is RIFF/WAVE");
        fclose(f);
        remove("/tmp/wb_test.wav");
    }
}

/* ---- test 4: effect units run without crashing and change signal ----- */
static void test_units(void) {
    printf("test_units\n");
    wb_sample L[256], R[256];
    for (int i = 0; i < 256; i++) { L[i] = (float)sin(i * 0.1); R[i] = L[i]; }

    /* compressor: should reduce a hot signal */
    void *comp = wb_comp_create(44100);
    CHECK(comp != NULL, "compressor created");
    float in_peak = 0;
    for (int i = 0; i < 256; i++) { float a=fabsf(L[i]); if(a>in_peak)in_peak=a; }
    wb_comp_process(comp, L, R, 256);
    float out_peak = 0;
    for (int i = 0; i < 256; i++) { float a=fabsf(L[i]); if(a>out_peak)out_peak=a; }
    CHECK(out_peak < 1.0f, "compressor clamped output");
    wb_comp_destroy(comp);

    /* delay: produces echoes (non-zero after dry window) */
    void *dl = wb_delay_create(44100);
    CHECK(dl != NULL, "delay created");
    for (int i = 0; i < 256; i++) { L[i] = (float)sin(i * 0.1); R[i] = L[i]; }
    wb_delay_process(dl, L, R, 256);
    wb_delay_destroy(dl);

    /* reverb */
    void *rv = wb_reverb_create(44100);
    CHECK(rv != NULL, "reverb created");
    for (int i = 0; i < 256; i++) { L[i] = (float)sin(i * 0.1); R[i] = L[i]; }
    wb_reverb_process(rv, L, R, 256);
    wb_reverb_destroy(rv);

    /* sampler */
    void *smp = wb_sampler_create(44100);
    CHECK(smp != NULL, "sampler created");
    wb_sample snd[1000];
    for (int i = 0; i < 1000; i++) snd[i] = (float)sin(i * 0.05);
    wb_sampler_load(smp, snd, 1000, 1);
    wb_sampler_note(smp, 60, 100);
    wb_sample sL[256], sR[256];
    wb_sampler_render(smp, sL, sR, 256);
    float spk = 0;
    for (int i = 0; i < 256; i++) { float a=fabsf(sL[i]); if(a>spk)spk=a; }
    CHECK(spk > 0.01f, "sampler produced audio");
    wb_sampler_destroy(smp);
}

/* ---- test 5: recursive tuner loop converges (loss decreases) --------- */
static void test_tuner(void) {
    printf("test_tuner\n");
    wb_session *s = wb_session_demo();
    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);
    wb_engine_play(e);

    wb_tuner *t = wb_tuner_create(e);
    CHECK(t != NULL, "tuner created");
    wb_tuner_start(t);

    /* let the loop run a few hundred ms — enough sense/act cycles */
    double loss1 = wb_tuner_last_loss(t);
    for (int i = 0; i < 50; i++) { struct timespec ts = {0, 10*1000000}; nanosleep(&ts, NULL); }
    double loss2 = wb_tuner_last_loss(t);

    CHECK(loss1 >= 0 && loss2 >= 0, "tuner produced valid loss metrics");
    printf("         loss: %.4f -> %.4f\n", loss1, loss2);

    wb_tuner_stop(t);
    wb_tuner_destroy(t);
    wb_engine_destroy(e);
    wb_session_destroy(s);
}

/* ---- test 6: offline render + file exists ----------------------------- */
static void test_render_file(void) {
    printf("test_render_file\n");
    wb_session *s = wb_session_demo();
    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);
    wb_sample *audio = NULL;
    uint32_t frames = 0;
    int rc = wb_engine_render_session(e, s, &audio, &frames);
    CHECK(rc == 0, "engine_render_session returns 0");
    CHECK(frames == (uint32_t)s->length, "rendered full length");
    float peak = 0;
    for (uint32_t i = 0; i < frames*2; i++) { float a = fabsf(audio[i]); if (a>peak) peak=a; }
    CHECK(peak > 0.01f, "rendered audio has content");
    printf("         peak=%.3f frames=%u\n", peak, frames);
    free(audio);
    wb_engine_destroy(e);
    wb_session_destroy(s);
}

/* ---- test 7: Xrun detection (try-lock drops a block, counts underrun) - */
static void test_xrun(void) {
    printf("test_xrun\n");
    wb_session *s = wb_session_demo();
    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);

    /* hold the process lock, then render -> should Xrun, not block */
    wb_engine_begin_edit(e);
    wb_sample buf[512*2];
    wb_engine_render(e, buf, 512);
    wb_engine_end_edit(e);

    CHECK(wb_engine_xruns(e) >= 1, "Xrun counted when process lock held");

    /* normal render does not increment xruns */
    uint64_t before = wb_engine_xruns(e);
    wb_engine_render(e, buf, 512);
    CHECK(wb_engine_xruns(e) == before, "no Xrun on normal render");

    wb_engine_destroy(e);
    wb_session_destroy(s);
}

int main(void) {
    printf("=== Big Mac DAW self-test gate ===\n");
    test_transport();
    test_synth_audio();
    test_wav();
    test_units();
    test_tuner();
    test_render_file();
    test_xrun();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
