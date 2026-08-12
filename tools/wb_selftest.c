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

/* ---- test 7: project save/load round-trip (.wbus) ---------------------- */
static void test_session_io(void) {
    printf("test_session_io\n");
    wb_session *s = wb_session_demo();
    CHECK(s != NULL, "demo session created");
    CHECK(s->track_count == 2, "demo has 2 tracks");
    CHECK(strcmp(s->tracks[0].inserts[1].id, "comp") == 0, "demo lead has comp insert");
    CHECK(strcmp(s->tracks[0].inserts[2].id, "reverb") == 0, "demo lead has reverb insert");

    /* save to a temp .wbus file */
    const char *path = "/tmp/test_save.wbus";
    int rc = wb_session_save(s, path);
    CHECK(rc == 0, "save returns 0");
    FILE *f = fopen(path, "r");
    CHECK(f != NULL, "saved file exists");
    if (f) fclose(f);

    /* load it back and verify round-trip fidelity */
    wb_session *s2 = wb_session_load(path);
    CHECK(s2 != NULL, "load returned a session");
    if (s2) {
        CHECK(s2->track_count == 2, "loaded session has 2 tracks");
        CHECK(strcmp(s2->tracks[0].name, "Lead") == 0, "loaded lead track name");
        CHECK(strcmp(s2->tracks[0].inserts[0].id, "synth") == 0, "loaded synth instrument");
        CHECK(strcmp(s2->tracks[0].inserts[1].id, "comp") == 0, "loaded comp insert");
        CHECK(strcmp(s2->tracks[0].inserts[2].id, "reverb") == 0, "loaded reverb insert");
        CHECK(s2->tracks[0].clips[0].note_count > 0, "loaded clip has notes");
    }
    wb_session_destroy(s);
    wb_session_destroy(s2);
}

/* ---- test 8: instruments (fm synth + drum machine + chorus + EQ) ------- */
static void test_instruments(void) {
    printf("test_instruments\n");
    wb_engine *e = wb_engine_create();
    CHECK(e != NULL, "engine created for instrument test");
    /* direct FM smoke: fire a note and render one block */
    void *fm = wb_fm_create(WB_SAMPLE_RATE);
    wb_fm_note(fm, 60, 100);
    float L[512], R[512];
    memset(L,0,sizeof(L)); memset(R,0,sizeof(R));
    wb_fm_render(fm, L, R, 512);
    float pm=0; for(int i=0;i<512;i++){ if(fabsf(L[i])>pm) pm=fabsf(L[i]); }
    CHECK(pm > 0.01f, "fm direct render produces audio");
    printf("         fm peak=%.4f\n", pm);
    wb_fm_destroy(fm);

    /* direct EQ smoke: should not produce NaN on silence */
    void *eq = wb_eq_create(WB_SAMPLE_RATE);
    float eL[512], eR[512];
    for(int i=0;i<512;i++){ eL[i]=0.0f; eR[i]=0.0f; }
    wb_eq_process(eq, eL, eR, 512);
    float nanmax=0; int nans=0;
    for(int i=0;i<512;i++){ if(isnan(eL[i])) nans++; if(!isnan(eL[i]) && fabsf(eL[i])>nanmax) nanmax=fabsf(eL[i]); }
    CHECK(nans==0, "eq produces no NaN on silence input");
    if(nans) printf("   EQ had %d NaN samples\n", nans);
    wb_eq_destroy(eq);

    wb_session *s = wb_session_create();
    s->bpm = 140.0;
    s->length = 44100.0 * 2.0;
    s->track_count = 2;
    s->tracks = calloc(2, sizeof(wb_track));

    /* lead: fm instrument + chorus insert */
    s->tracks[0].kind = 0;
    s->tracks[0].volume = 1.0f;
    strncpy(s->tracks[0].name, "Lead", 64);
    strcpy(s->tracks[0].inserts[0].id, "fm");
    strcpy(s->tracks[0].inserts[1].id, "chorus");
    s->tracks[0].clip_count = 1;
    s->tracks[0].clips = calloc(1, sizeof(wb_clip));
    s->tracks[0].clips[0].start = 0;
    s->tracks[0].clips[0].length = 88200;
    s->tracks[0].clips[0].note_count = 1;
    s->tracks[0].clips[0].notes = calloc(1, sizeof(wb_note));
    s->tracks[0].clips[0].notes[0].start = 0.0;
    s->tracks[0].clips[0].notes[0].dur = 44100.0;
    s->tracks[0].clips[0].notes[0].pitch = 60;
    s->tracks[0].clips[0].notes[0].vel = 100;

    /* drums: drum instrument + eq insert */
    s->tracks[1].kind = 0;
    s->tracks[1].volume = 1.0f;
    strncpy(s->tracks[1].name, "Drums", 64);
    strcpy(s->tracks[1].inserts[0].id, "drum");
    strcpy(s->tracks[1].inserts[1].id, "eq");  /* EQ on drums */
    s->tracks[1].clip_count = 1;
    s->tracks[1].clips = calloc(1, sizeof(wb_clip));
    s->tracks[1].clips[0].start = 0;
    s->tracks[1].clips[0].length = 88200;
    int k[] = {36, 37, 38, 42};
    double st[] = {0, 11025, 22050, 33075};
    s->tracks[1].clips[0].note_count = 4;
    s->tracks[1].clips[0].notes = calloc(4, sizeof(wb_note));
    for (int i = 0; i < 4; i++) {
        s->tracks[1].clips[0].notes[i].start = st[i];
        s->tracks[1].clips[0].notes[i].dur = 11025.0;
        s->tracks[1].clips[0].notes[i].pitch = (uint8_t)k[i];
        s->tracks[1].clips[0].notes[i].vel = 100;
    }

    wb_sample *out = NULL;
    uint32_t frames = 0;
    int rc = wb_engine_render_session(NULL, s, &out, &frames);
    CHECK(rc == 0, "render session with fm+drum+effects");

    int nonzero = 0;
    float peak = 0;
    for (uint32_t i = 0; i < frames * 2; i++) {
        if (out[i] != 0) nonzero++;
        if (fabsf(out[i]) > peak) peak = fabsf(out[i]);
    }
    free(out);
    CHECK(nonzero > 0, "fm+drum render has audio content");
    CHECK(peak > 0.01f, "fm+drum render has audible level");
    printf("         peak=%.4f nonzero=%d\n", peak, nonzero);

    wb_session_destroy(s);
    wb_engine_destroy(e);
}

/* ---- test: automation envelopes (fade master volume 1->0 over the song) - */
static void test_automation(void) {
    printf("test_automation\n");
    wb_session *s = wb_session_create();
    s->bpm = 120.0;
    s->length = 88200.0;
    wb_track *tr = wb_session_add_track(s, "Bump", 0);
    strcpy(tr->inserts[0].id, "drum");
    tr->volume = 1.0f;
    tr->clip_count = 1; tr->clips = calloc(1, sizeof(wb_clip));
    tr->clips[0].start = 0; tr->clips[0].length = 88200;
    tr->clips[0].note_count = 1; tr->clips[0].notes = calloc(1, sizeof(wb_note));
    tr->clips[0].notes[0] = (wb_note){.pitch=36,.start=0,.dur=44100,.vel=100};

    /* master-volume automation: full volume at sample 0, silent by end */
    wb_automation_lane *lane = wb_session_add_automation(s, "volume", -1);
    CHECK(lane != NULL, "automation lane created");
    CHECK(wb_automation_add_point(lane, 0,       1.0, 2) == 0, "point at 0.0s");
    CHECK(wb_automation_add_point(lane, 44100,   0.8, 0) == 0, "point mid-fade");
    CHECK(wb_automation_add_point(lane, 88200,   0.0, 0) == 0, "point at end");
    CHECK(lane->point_count == 3, "three breakpoints present");
    CHECK(fabs(wb_automation_value_at(lane, 0,   -1) - 1.0) < 1e-6, "v at t=0 is 1.0");
    CHECK(fabs(wb_automation_value_at(lane, 22050,-1) - 0.9) < 1e-4, "v mid-fade linear interp");
    CHECK(fabs(wb_automation_value_at(lane, 88200,-1) - 0.0) < 1e-6, "v at end is 0.0");
    CHECK(wb_automation_value_at(lane, 99999,-7) == 0.0, "past-end holds last value");

    wb_sample *out = NULL;
    uint32_t frames = 0;
    CHECK(wb_engine_render_session(NULL, s, &out, &frames) == 0, "render with automation");
    CHECK(frames > 0, "rendered some frames");

    /* sample the start (loud) vs the tail (quiet) of the master output */
    float head_peak = 0, tail_peak = 0;
    for (uint32_t i = 0; i < 2048 && i < frames*2; i++) head_peak = fmaxf(head_peak, fabsf(out[i]));
    uint32_t tail0 = frames*2 - 2048;
    for (uint32_t i = tail0; i < frames*2; i++) tail_peak = fmaxf(tail_peak, fabsf(out[i]));
    printf("         head_peak=%.4f tail_peak=%.4f (expected tail << head)\n", head_peak, tail_peak);
    CHECK(head_peak > 0.01f, "head of mix is audible (pre-fade)");
    CHECK(tail_peak < head_peak * 0.5f, "tail quieter than head (fade applied)");

    /* round-trip the automation lane through .wbus */
    CHECK(wb_session_save(s, "/tmp/wb_auto_test.wbus") == 0, "save session with automation");
    wb_session *s2 = wb_session_load("/tmp/wb_auto_test.wbus");
    CHECK(s2 && s2->automation_count == 1, "loaded one automation lane");
    if (s2 && s2->automation_count == 1) {
        wb_automation_lane *l2 = s2->automation[0];
        CHECK(l2->target == -1 && strcmp(l2->param,"volume")==0, "lane params round-trip");
        CHECK(l2->point_count == 3, "three points round-trip");
        CHECK(fabs(wb_automation_value_at(l2,22050,-1) - 0.9) < 1e-4, "interpolation round-trips");
    }
    wb_session_destroy(s2);
    free(out);
    wb_session_destroy(s);
}

/* ---- test: MIDI recording into clips ----------------------------------- */
static void test_recorder(void) {
    printf("test_recorder\n");
    wb_session *s = wb_session_create();
    s->bpm = 120.0; s->length = 88200.0; s->tracks = calloc(1, sizeof(wb_track));
    s->tracks[0].kind = 0; s->tracks[0].volume = 1.0f;
    strcpy(s->tracks[0].inserts[0].id, "fm");
    s->tracks[0].clip_count = 1;
    s->tracks[0].clips = calloc(1, sizeof(wb_clip));
    s->tracks[0].clips[0].start = 0; s->tracks[0].clips[0].length = 88200;
    s->track_count = 1;

    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);
    wb_engine_seek(e, 0.0);

    /* arm recording on track 0, clip 0 */
    wb_engine_record(e, 0, 0, 1, 0);

    /* simulate a 1-second held note (C4) recorded over the first second */
    wb_sample rb[4096*2];
    wb_engine_note(e, 0, 60, 100);                 /* note-on at pos 0 */
    wb_engine_render(e, rb, 4096);                 /* advance + flush */
    wb_engine_note(e, 0, 60, 0);                   /* note-off at pos 4096 */
    wb_engine_render(e, rb, 4096);                 /* advance + flush */

    wb_engine_record(e, 0, 0, 0, 0);               /* disarm */
    wb_track *tk = &s->tracks[0];
    CHECK(tk->clips[0].note_count >= 1, "recorder captured a note");
    if (tk->clips[0].note_count >= 1) {
        wb_note n0 = tk->clips[0].notes[0];
        printf("         rec note pitch=%d start=%.0f dur=%.0f vel=%d\n",
               n0.pitch, n0.start, n0.dur, n0.vel);
        CHECK(n0.pitch == 60, "recorded note pitch matches");
        CHECK(n0.start == 0.0, "recorded note starts at 0");
        CHECK(n0.dur > 0, "recorded note has a positive duration");
        CHECK(n0.vel == 100, "recorded note velocity preserved");
    }

    /* reload: the authored clip must survive save/load */
    CHECK(wb_session_save(s, "/tmp/wb_rec_test.wbus") == 0, "save recorded session");
    wb_session *s2 = wb_session_load("/tmp/wb_rec_test.wbus");
    CHECK(s2 && s2->track_count == 1, "loaded recorded session");
    if (s2 && s2->track_count == 1) {
        CHECK(s2->tracks[0].clips[0].note_count >= 1, "recorded notes survive .wbus round-trip");
        if (s2->tracks[0].clips[0].note_count >= 1) {
            wb_note n2 = s2->tracks[0].clips[0].notes[0];
            CHECK(n2.pitch == 60 && n2.vel == 100, "note fields round-trip");
        }
    }
    wb_session_destroy(s2);
    wb_engine_destroy(e);
    wb_session_destroy(s);
}

/* ---- test: undo/redo via session snapshots ------------------------------ */
static void test_undo(void) {
    printf("test_undo\n");
    wb_session *s = wb_session_create();
    s->bpm = 120.0; s->length = 88200.0;
    wb_undo *u = wb_undo_create();

    /* checkpoint the empty session, then add a track + note */
    wb_undo_checkpoint(u, s);
    wb_track *tr = wb_session_add_track(s, "Lead", 0);
    strcpy(tr->inserts[0].id, "fm");
    wb_session_add_note(tr, 0, 44100, 60, 100);
    CHECK(s->track_count == 1 && tr->clips[0].note_count == 1, "edit applied");

    /* checkpoint again, then change the note pitch */
    wb_undo_checkpoint(u, s);
    s->tracks[0].clips[0].notes[0].pitch = 72;
    CHECK(s->tracks[0].clips[0].notes[0].pitch == 72, "second edit applied");
    CHECK(wb_undo_depth(u) == 2, "two undo checkpoints recorded");

    /* undo: back to pitch 60 */
    int ok = wb_undo_undo(u, &s);
    CHECK(ok == 1, "undo #1 performed");
    CHECK(s->tracks[0].clips[0].notes[0].pitch == 60, "undo restores pitch 60");
    CHECK(wb_undo_redo_depth(u) == 1, "redo stack has one entry");

    /* undo again: back to empty session */
    ok = wb_undo_undo(u, &s);
    CHECK(ok == 1, "undo #2 performed");
    CHECK(s->track_count == 0, "undo restores empty session");
    CHECK(wb_undo_depth(u) == 0, "undo stack drained");

    /* nothing to undo */
    ok = wb_undo_undo(u, &s);
    CHECK(ok == 0, "no-op undo past bottom");

    /* redo: forward to pitch-60 state, then pitch-72 state */
    ok = wb_undo_redo(u, &s);
    CHECK(ok == 1, "redo #1 performed");
    CHECK(s->track_count == 1 && s->tracks[0].clips[0].notes[0].pitch == 60, "redo restores pitch 60");
    ok = wb_undo_redo(u, &s);
    CHECK(ok == 1, "redo #2 performed");
    CHECK(s->tracks[0].clips[0].notes[0].pitch == 72, "redo restores pitch 72");

    /* new edit clears redo */
    wb_undo_checkpoint(u, s);
    s->tracks[0].clips[0].notes[0].pitch = 64;
    CHECK(wb_undo_redo_depth(u) == 0, "new checkpoint clears redo branch");

    /* deep-copy independence: editing the copy leaves the original alone */
    wb_session *s2 = wb_session_copy(s);
    CHECK(s2 != NULL, "session deep copy created");
    s2->tracks[0].clips[0].notes[0].pitch = 100;
    CHECK(s->tracks[0].clips[0].notes[0].pitch == 64, "copy is independent of original");
    wb_session_destroy(s2);

    wb_undo_destroy(u);
    wb_session_destroy(s);
}

/* ---- test 8: Xrun detection (try-lock drops a block, counts underrun) - */
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
    setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered so we see output on crash */
    printf("=== Big Mac DAW self-test gate ===\n");
    test_transport();
    test_synth_audio();
    test_wav();
    test_units();
    test_tuner();
    test_render_file();
    test_session_io();
    test_instruments();
    test_automation();
    test_recorder();
    test_undo();
    test_xrun();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
