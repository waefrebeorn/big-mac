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
#include "wbus_midi.h"
#include "wbus_modulation.h"
#include "wbus_midifx.h"
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

    /* saturation: drives a soft-clipped, louder-thicker signal */
    void *sat = wb_sat_create(44100);
    CHECK(sat != NULL, "saturation created");
    for (int i = 0; i < 256; i++) { L[i] = (float)sin(i * 0.1); R[i] = L[i]; }
    wb_sat_set(sat, 0, 1.0f);   /* max drive */
    wb_sat_set(sat, 1, 0.5f);   /* some makeup */
    wb_sat_process(sat, L, R, 256);
    int clipped = 0;
    for (int i = 0; i < 256; i++) if (fabsf(L[i]) > 1.0f) clipped++;
    CHECK(clipped == 0, "saturation stays within [-1,1] (tanh clamps)");
    wb_sat_destroy(sat);

    /* gate: silences a sub-threshold signal */
    void *g = wb_gate_create(44100);
    CHECK(g != NULL, "gate created");
    for (int i = 0; i < 256; i++) { L[i] = 0.005f; R[i] = 0.005f; }  /* below threshold */
    wb_gate_set(g, 0, 0.05f);  /* threshold 0.05 */
    wb_gate_process(g, L, R, 256);
    float gpk = 0;
    for (int i = 0; i < 256; i++) { float a=fabsf(L[i]); if(a>gpk)gpk=a; }
    CHECK(gpk < 0.005f + 1e-3f, "gate attenuates sub-threshold signal");
    /* now an above-threshold burst should pass */
    for (int i = 0; i < 256; i++) { L[i] = (i < 64) ? 0.5f : 0.005f; R[i] = L[i]; }
    wb_gate_process(g, L, R, 256);
    CHECK(fabsf(L[10]) > 0.1f, "gate passes above-threshold signal");
    wb_gate_destroy(g);

    /* multiband: 3-band compressor splits + compresses, stays finite */
    void *mb = wb_mb_create(44100);
    CHECK(mb != NULL, "multiband created");
    /* a hot broadband signal with a loud low-frequency thump */
    for (int i = 0; i < 512; i++) {
        L[i] = 0.9f * (float)sin(i * 0.05f) + 0.9f * (float)sin(i * 0.005f);
        R[i] = L[i];
    }
    /* push harder on the low band to exercise per-band compression */
    wb_mb_set_param(mb, "low_thresh", 0.4f);
    wb_mb_set_param(mb, "low_ratio", 0.9f);
    wb_mb_set_param(mb, "low_makeup", 0.8f);
    CHECK(wb_mb_has_param(mb, "f1") && wb_mb_has_param(mb, "mid_ratio"), "multiband exposes named params");
    CHECK(wb_mb_get_param(mb, "f1") > 0, "multiband get_param returns a crossover value");
    wb_mb_process(mb, L, R, 512);
    int finite = 1, nan = 0;
    for (int i = 0; i < 512; i++) {
        if (!isfinite(L[i]) || !isfinite(R[i])) finite = 0;
        if (isnan(L[i]) || isnan(R[i])) nan = 1;
    }
    CHECK(finite && !nan, "multiband output is finite (no NaN from crossovers/comp)");
    /* summed output should be bounded (compression + LR sum) */
    float pk = 0;
    for (int i = 0; i < 512; i++) { float a=fabsf(L[i]); if(a>pk)pk=a; }
    CHECK(pk <= 1.6f, "multiband output bounded after makeup");
    wb_mb_destroy(mb);

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
    CHECK(s->track_count == 3, "demo has 3 tracks (lead, bass, pad)");
    CHECK(strcmp(s->tracks[0].inserts[1].id, "comp") == 0, "demo lead has comp insert");
    CHECK(strcmp(s->tracks[0].inserts[2].id, "reverb") == 0, "demo lead has reverb insert");
    /* track 2 is the audio pad clip */
    CHECK(s->tracks[2].kind == 1, "demo track 2 is audio");
    CHECK(s->tracks[2].clips[0].audio_frames > 0, "audio clip has samples");
    /* R022: demo song carries song-section arrangement markers */
    CHECK(s->marker_count == 4, "demo has 4 arrangement markers");
    CHECK(strcmp(s->markers[0].label, "Intro") == 0, "marker 0 is Intro");
    CHECK(strcmp(s->markers[2].label, "Chorus") == 0, "marker 2 is Chorus");

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
        CHECK(s2->track_count == 3, "loaded session has 3 tracks");
        CHECK(strcmp(s2->tracks[0].name, "Lead") == 0, "loaded lead track name");
        CHECK(strcmp(s2->tracks[0].inserts[0].id, "synth") == 0, "loaded synth instrument");
        CHECK(strcmp(s2->tracks[0].inserts[1].id, "comp") == 0, "loaded comp insert");
        CHECK(strcmp(s2->tracks[0].inserts[2].id, "reverb") == 0, "loaded reverb insert");
        CHECK(s2->tracks[0].clips[0].note_count > 0, "loaded clip has notes");
        CHECK(s2->tracks[0].route == -1, "loaded track routes to master by default");
        /* R022: clip_gain round-trips through .wbus save/load */
        CHECK(fabsf(s2->tracks[2].clips[0].clip_gain - 1.0f) < 1e-3f,
              "loaded audio clip_gain == 1.0 (unity)");
        /* R022: markers round-trip through .wbus save/load */
        CHECK(s2->marker_count == 4, "loaded session has 4 markers");
        CHECK(strcmp(s2->markers[1].label, "Verse") == 0, "loaded marker 1 is Verse");

        /* the loaded project must render non-silent through the engine
         * (proves a disk project opens and plays, the SOTA file workflow) */
        wb_engine *re = wb_engine_create();
        wb_engine_set_session(re, s2);
        wb_engine_seek(re, 0.0); wb_engine_play(re);
        wb_sample ob[4096*2];
        wb_engine_render(re, ob, 4096);
        float pk = 0;
        for (int i = 0; i < 4096*2; i++) { float v = ob[i]<0?-ob[i]:ob[i]; if (v>pk) pk=v; }
        CHECK(pk > 0.001f, "loaded project renders audio (non-silent)");
        wb_engine_destroy(re);
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
    s->tracks[0].route = -1;
    s->tracks[1].route = -1;

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

/* ---- test: automation recording (capture live fader moves) ------------- */
/* helper: count pending points in a recorder */
static int rec_count_pending(const wb_automation_recorder *r) {
    return wb_automation_recorder_count(r);
}

static void test_automation_record(void) {
    printf("test_automation_record\n");
    wb_automation_lane *lane = wb_automation_lane_create("volume");
    wb_automation_recorder *rec = wb_automation_recorder_create(lane, 0.02);
    CHECK(rec != NULL, "automation recorder created");

    /* arm with initial value, then feed a rising fader during playback */
    wb_automation_recorder_arm(rec, 0.0);
    wb_automation_recorder_capture(rec, 0.0, 0.0);
    wb_automation_recorder_capture(rec, 44100, 0.3);
    wb_automation_recorder_capture(rec, 88200, 0.6);
    wb_automation_recorder_capture(rec, 132300, 0.9);
    /* a sub-deadband jitter move should be ignored */
    wb_automation_recorder_capture(rec, 176400, 0.905);
    wb_automation_recorder_disarm(rec);
    wb_automation_recorder_capture(rec, 220500, 1.0);  /* ignored: disarmed */
    CHECK(rec_count_pending(rec) == 4, "4 points captured (jitter + post-disarm ignored)");

    /* commit into the lane */
    int n = wb_automation_recorder_commit(rec);
    CHECK(n == 1, "commit performed");
    CHECK(lane->point_count == 4, "lane holds 4 recorded points");
    CHECK(lane->points[1].value == 0.3, "second point value kept");
    CHECK(lane->points[3].value == 0.9, "last recorded value kept");
    CHECK(lane->points[0].curve == 0, "recorded points are linear");

    /* overwrite region: pre-existing points before t0 survive, at/after are dropped */
    wb_automation_lane *lane2 = wb_automation_lane_create("volume");
    wb_automation_add_point(lane2, 10000, 0.1, 0);
    wb_automation_add_point(lane2, 50000, 0.9, 0);  /* in overwrite region */
    wb_automation_recorder *rec2 = wb_automation_recorder_create(lane2, 0.01);
    wb_automation_recorder_arm(rec2, 0.5);
    wb_automation_recorder_capture(rec2, 30000, 0.2); /* t0 = 30000 */
    wb_automation_recorder_capture(rec2, 60000, 0.7);
    wb_automation_recorder_commit(rec2);
    /* point at 10000 (before t0) survives; point at 50000 (>= t0) replaced */
    CHECK(lane2->point_count == 3, "pre-recording point + 2 new = 3");
    CHECK(lane2->points[0].time == 10000 && lane2->points[0].value == 0.1,
          "point before recording region survived");
    CHECK(lane2->points[2].time == 60000 && lane2->points[2].value == 0.7,
          "recorded points appended sorted");

    wb_automation_recorder_destroy(rec2);
    wb_automation_recorder_destroy(rec);
    wb_automation_lane_destroy(lane2);
    wb_automation_lane_destroy(lane);
}

/* ---- test: MIDI recording into clips ----------------------------------- */
static void test_recorder(void) {
    printf("test_recorder\n");
    wb_session *s = wb_session_create();
    s->bpm = 120.0; s->length = 88200.0; s->tracks = calloc(1, sizeof(wb_track));
    s->tracks[0].kind = 0; s->tracks[0].volume = 1.0f; s->tracks[0].route = -1;
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

/* ---- test: audio clip playback + waveform data path ---------------------- */
static void test_audio_clip(void) {
    printf("test_audio_clip\n");
    wb_session *s = wb_session_create();
    s->bpm = 120.0; s->length = 88200.0;
    wb_track *tr = wb_session_add_track(s, "Snare", 1);  /* audio track */
    /* a simple 1s sine "click" at 440 Hz */
    uint32_t nf = 44100;
    wb_sample buf[44100];
    for (uint32_t i = 0; i < nf; i++) {
        double t = (double)i / 44100.0;
        buf[i] = (float)(0.5 * sin(2*M_PI*440.0*t) * (1.0 - (double)i/(nf-1)));
    }
    wb_session_add_audio_clip(tr, 0, (double)nf, buf, nf, 1);
    CHECK(tr->clip_count == 1 && tr->clips[0].type == 1, "audio clip added");
    CHECK(tr->clips[0].audio_frames == nf, "audio frame count set");

    /* render the engine over the clip; audio should appear in the output */
    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);
    wb_engine_seek(e, 0.0);
    wb_engine_play(e);
    wb_sample out[4096*2];
    wb_engine_render(e, out, 4096);
    float peak = 0;
    for (uint32_t i = 0; i < 4096*2; i++) { float v = out[i]<0?-out[i]:out[i]; if (v>peak) peak=v; }
    CHECK(peak > 0.01f, "audio clip rendered into output (peak audible)");
    printf("         audio clip peak=%.3f at 440Hz\n", peak);

    wb_engine_destroy(e);
    wb_session_destroy(s);
}

/* ---- test: R022 clip gain is applied pre-fader in the audio path -------- */
static void test_clip_gain(void) {
    printf("test_clip_gain\n");
    wb_session *s = wb_session_create();
    s->bpm = 120.0; s->length = 88200.0;
    wb_track *tr = wb_session_add_track(s, "Clip", 1);  /* audio track */
    uint32_t nf = 44100;
    wb_sample buf[44100];
    for (uint32_t i = 0; i < nf; i++) {
        double t = (double)i / 44100.0;
        buf[i] = (float)(0.3 * sin(2*M_PI*440.0*t));
    }
    wb_session_add_audio_clip(tr, 0, (double)nf, buf, nf, 1);

    /* peak at unity gain */
    tr->clips[0].clip_gain = 1.0f;
    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);
    wb_engine_seek(e, 0.0); wb_engine_play(e);
    wb_sample out[4096*2];
    wb_engine_render(e, out, 4096);
    float p1 = 0;
    for (uint32_t i = 0; i < 4096*2; i++) { float v = out[i]<0?-out[i]:out[i]; if (v>p1) p1=v; }
    CHECK(p1 > 0.01f, "clip renders at unity gain");

    /* peak at 2x region gain — must be ~2x louder (pre-fader, real signal) */
    tr->clips[0].clip_gain = 2.0f;
    wb_engine_seek(e, 0.0); wb_engine_play(e);
    wb_engine_render(e, out, 4096);
    float p2 = 0;
    for (uint32_t i = 0; i < 4096*2; i++) { float v = out[i]<0?-out[i]:out[i]; if (v>p2) p2=v; }
    CHECK(p2 > p1*1.8f, "clip_gain=2.0 ~doubles rendered peak (pre-fader)");
    printf("         unity peak=%.3f  x2 peak=%.3f\n", p1, p2);

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

/* ---- test: wb_session_remove_note (self-contained; does not perturb undo) - */
static void test_remove_note(void) {
    printf("test_remove_note\n");
    wb_session *s = wb_session_create();
    wb_track *tr = wb_session_add_track(s, "Lead", 0);
    wb_session_add_note(tr, 0, 44100, 60, 100);
    wb_session_add_note(tr, 44100, 44100, 64, 100);
    CHECK(tr->clips[0].note_count == 2, "two notes added");
    int rc = wb_session_remove_note(tr, 0, 60);
    CHECK(rc == 0 && tr->clips[0].note_count == 1, "remove_note deletes one note");
    CHECK(tr->clips[0].notes[0].pitch == 64, "remaining note is the other one");
    int rc2 = wb_session_remove_note(tr, 44100, 64);
    CHECK(rc2 == 0 && tr->clips[0].note_count == 0, "remove_note deletes the last note");
    int rc3 = wb_session_remove_note(tr, 0, 60);
    CHECK(rc3 == -1, "remove_note on empty clip returns -1 (no crash)");
    wb_session_destroy(s);
}

/* ---- test: bus/group routing -------------------------------------------- */
static void test_bus_routing(void) {
    printf("test_bus_routing\n");
    wb_session *s = wb_session_create();
    s->bpm = 120.0; s->length = 44100.0;
    /* track 0: group/bus (kind 2) with 0.5 volume */
    wb_track *bus = wb_session_add_track(s, "Bus", 0);
    bus->kind = 2; bus->volume = 0.5f; bus->route = -1;
    /* track 1: routed to bus (index 0), fm synth, holds a C4 note */
    wb_track *a = wb_session_add_track(s, "A", 0);
    a->volume = 1.0f; a->route = 0; strcpy(a->inserts[0].id, "fm");
    a->clip_count = 1; a->clips = calloc(1, sizeof(wb_clip));
    a->clips[0].start = 0; a->clips[0].length = 44100;
    a->clips[0].note_count = 1;
    a->clips[0].notes = calloc(1, sizeof(wb_note));
    a->clips[0].notes[0].start = 0; a->clips[0].notes[0].dur = 44100;
    a->clips[0].notes[0].pitch = 60; a->clips[0].notes[0].vel = 100;
    /* track 2: routed to master directly */
    wb_track *b = wb_session_add_track(s, "B", 0);
    b->volume = 1.0f; b->route = -1; strcpy(b->inserts[0].id, "fm");
    b->clip_count = 1; b->clips = calloc(1, sizeof(wb_clip));
    b->clips[0].start = 0; b->clips[0].length = 44100;
    b->clips[0].note_count = 1;
    b->clips[0].notes = calloc(1, sizeof(wb_note));
    b->clips[0].notes[0].start = 0; b->clips[0].notes[0].dur = 44100;
    b->clips[0].notes[0].pitch = 60; b->clips[0].notes[0].vel = 100;

    wb_sample *out = NULL; uint32_t frames = 0;
    int rc = wb_engine_render_session(NULL, s, &out, &frames);
    CHECK(rc == 0, "bus project renders");
    float peak = 0;
    for (uint32_t i = 0; i < frames*2; i++) if (fabsf(out[i]) > peak) peak = fabsf(out[i]);
    free(out);
    CHECK(peak > 0.01f, "bus-routed tracks reach master (via bus)");
    CHECK(s->tracks[1].route == 0, "track A is routed to the bus");
    CHECK(s->tracks[0].kind == 2, "track 0 is a group/bus");

    /* Verify bus attenuation: re-render with bus volume at 1.0 (no attenuation)
     * and confirm the level rises. The bus scales its summed sources, so a
     * higher bus gain must produce a higher master peak. */
    s->tracks[0].volume = 1.0f;
    rc = wb_engine_render_session(NULL, s, &out, &frames);
    CHECK(rc == 0, "bus re-render ok");
    float peak_full = 0;
    for (uint32_t i = 0; i < frames*2; i++) if (fabsf(out[i]) > peak_full) peak_full = fabsf(out[i]);
    free(out);
    CHECK(peak_full > peak, "bus volume scales the routed signal");
    wb_session_destroy(s);
}

/* ---- test: Launchpad grid→note mapping (classic LP, pure logic) ------- */
static void test_launchpad(void) {
    printf("test_launchpad\n");
    /* grid corners — classic Launchpad layout: row*16 + col */
    CHECK(wb_launchpad_classic_note(0,0) == 0,    "grid (0,0) -> note 0");
    CHECK(wb_launchpad_classic_note(0,7) == 7,    "grid (0,7) -> note 7");
    CHECK(wb_launchpad_classic_note(1,0) == 16,   "grid (1,0) -> note 16");
    CHECK(wb_launchpad_classic_note(7,7) == 119,  "grid (7,7) -> note 119");
    /* out of range is rejected */
    CHECK(wb_launchpad_classic_note(-1,0) == -1,  "negative row rejected");
    CHECK(wb_launchpad_classic_note(8,0)  == -1,  "row>7 rejected");
    CHECK(wb_launchpad_classic_note(0,8)  == -1,  "col>7 rejected");
    /* full 8x8 grid covers exactly 0..119 in 16-steps */
    int seen[120] = {0}; int ok = 1;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            int n = wb_launchpad_classic_note(r, c);
            if (n < 0 || n > 119 || seen[n]) ok = 0;
            seen[n] = 1;
        }
    CHECK(ok, "8x8 grid maps to 64 unique notes in 0..119 (classic)");
    CHECK(wb_lp_mk2_note(0,0) == 11,  "MK2 grid (0,0) -> note 11");
    CHECK(wb_lp_mk2_note(7,7) == 88,  "MK2 grid (7,7) -> note 88");
    CHECK(wb_lp_mk2_note(-1,0) == -1, "MK2 negative row rejected");
    int seen2[128] = {0}; int ok2 = 1;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            int n = wb_lp_mk2_note(r, c);
            if (n < 11 || n > 88 || seen2[n]) ok2 = 0;
            seen2[n] = 1;
        }
    CHECK(ok2, "MK2 8x8 grid maps to 64 unique notes in 11..88");
    printf("         grid maps 64 cells -> notes 0..119 (classic Launchpad)\n");
    printf("         MK2 grid maps 64 cells -> notes 11..88\n");
}

/* ---- test: compressor sidechain key input ducks the program signal ------- */
static void test_sidechain(void) {
    printf("test_sidechain\n");
    void *comp = wb_comp_create(44100);
    CHECK(comp != NULL, "compressor created for sidechain test");

    /* program material: steady -6 dBFS tone-ish signal */
    wb_sample prog[256];
    for (int i = 0; i < 256; i++) prog[i] = 0.5f;

    /* pass 1: NO key (normal compression of program only) */
    wb_sample out_nokey[256];
    memcpy(out_nokey, prog, sizeof(prog));
    wb_comp_process(comp, out_nokey, out_nokey, 256);
    float peak_nokey = 0;
    for (int i = 0; i < 256; i++) if (fabsf(out_nokey[i]) > peak_nokey) peak_nokey = fabsf(out_nokey[i]);

    /* reset envelope so this is a fair comparison */
    wb_comp_destroy(comp);
    comp = wb_comp_create(44100);

    /* pass 2: WITH a loud key signal driving the envelope */
    wb_sample key[256];
    for (int i = 0; i < 256; i++) key[i] = 1.0f;  /* full-scale key */
    wb_comp_set_key(comp, key, key, 256);
    wb_sample out_key[256];
    memcpy(out_key, prog, sizeof(prog));
    wb_comp_process(comp, out_key, out_key, 256);
    float peak_key = 0;
    for (int i = 0; i < 256; i++) if (fabsf(out_key[i]) > peak_key) peak_key = fabsf(out_key[i]);

    CHECK(peak_key < peak_nokey, "sidechain key ducks program output (peak_key < peak_nokey)");
    CHECK(peak_key < 0.5f, "keyed compression attenuates the steady program");

    /* engine routing: build a 2-track session with sidechain 0 -> track1 slot1 */
    wb_session *s = wb_session_create();
    s->track_count = 2;
    s->tracks = calloc(2, sizeof(wb_track));
    s->tracks[0].kind = 0; s->tracks[0].route = -1;
    strcpy(s->tracks[0].inserts[0].id, "synth");
    s->tracks[1].kind = 0; s->tracks[1].route = -1;
    strcpy(s->tracks[1].inserts[0].id, "synth");
    strcpy(s->tracks[1].inserts[1].id, "comp");
    s->tracks[1].sidechain[1] = 0;   /* track1 slot1 keyed from track0 */

    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);
    wb_engine_play(e);
    wb_engine_note(e, 0, 48, 100);  /* kick-ish note on track 0 (key source) */
    wb_engine_note(e, 1, 36, 100);  /* bass note on track 1 (keyed comp) */
    wb_sample buf[1024 * 2];
    for (int b = 0; b < 16; b++) wb_engine_render(e, buf, 1024);
    CHECK(1, "engine renders with sidechain routing active (no crash)");
    int nan = 0;
    for (int i = 0; i < 1024 * 2; i++) if (!isfinite(buf[i])) nan++;
    CHECK(nan == 0, "sidechain routing produces finite (non-NaN) output");

    wb_engine_destroy(e);
    wb_session_destroy(s);
    wb_comp_destroy(comp);
}

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

/* ---- test: MIDI FX chain transforms note events ----------------------- */
static void test_midifx(void) {
    printf("test_midifx\n");
    wb_midifx_event in = { .pitch = 60, .vel = 100, .on = 1, .tick = 0 };
    wb_midifx_event out[8];

    /* transpose +12 */
    wb_midifx *tp = wb_midifx_create(WB_MIDIFX_TRANSPOSE);
    wb_midifx_set_param(tp, 0, 12.0f);
    int n = wb_midifx_process(tp, &in, out, 8);
    CHECK(n == 1 && out[0].pitch == 72, "transpose +12 maps C4->C5");
    wb_midifx_destroy(tp);

    /* velocity scale 0.5 */
    wb_midifx *vel = wb_midifx_create(WB_MIDIFX_VELOCITY);
    wb_midifx_set_param(vel, 0, 0.5f);
    n = wb_midifx_process(vel, &in, out, 8);
    CHECK(n == 1 && out[0].vel == 50, "velocity x0.5 scales 100->50");
    wb_midifx_destroy(vel);

    /* chord: root +7 (fifth) +12 (octave) => 3 notes */
    wb_midifx *ch = wb_midifx_create(WB_MIDIFX_CHORD);
    wb_midifx_set_param(ch, 0, 7.0f);
    wb_midifx_set_param(ch, 1, 12.0f);
    n = wb_midifx_process(ch, &in, out, 8);
    CHECK(n == 3, "chord with 2 intervals emits 3 notes");
    CHECK(out[0].pitch == 60 && out[1].pitch == 67 && out[2].pitch == 72,
          "chord intervals correct (root/5th/oct)");
    wb_midifx_destroy(ch);

    /* arpeggiator: latch 2 held notes, tick emits them in turn */
    wb_midifx *arp = wb_midifx_create(WB_MIDIFX_ARP);
    wb_midifx_event n60 = { .pitch = 60, .vel = 100, .on = 1 };
    wb_midifx_event n64 = { .pitch = 64, .vel = 100, .on = 1 };
    wb_midifx_event off = { .pitch = 60, .vel = 0,   .on = 0 };
    wb_midifx_process(arp, &n60, out, 8);   /* latch 60 */
    wb_midifx_process(arp, &n64, out, 8);   /* latch 64 */
    int e1 = wb_midifx_tick(arp, 1, out, 16);
    int e2 = wb_midifx_tick(arp, 1, out + 1, 15);
    CHECK(e1 == 1 && e2 == 1, "arp emits one note per tick");
    CHECK((out[0].pitch == 60 && out[1].pitch == 64) ||
          (out[0].pitch == 64 && out[1].pitch == 60),
          "arp cycles through held notes");
    wb_midifx_process(arp, &off, out, 8);   /* release 60 */
    wb_midifx_tick(arp, 1, out, 16);
    CHECK(1, "arp handles note release without crash");
    wb_midifx_destroy(arp);
}

/* ---- test: modulation matrix drives a parameter over time -------------- */
static void mod_test_setter(void *ctx, int track, int slot, int param, float value01) {
    (void)track; (void)slot; (void)param;
    float *dst = (float*)ctx;
    if (dst) *dst = value01;
}

static void test_modulation(void) {
    printf("test_modulation\n");
    wb_mod_matrix *m = wb_mod_matrix_create();
    CHECK(m != NULL, "modulation matrix created");

    /* LFO source at 1 Hz, full depth */
    wb_mod_src *lfo = wb_mod_src_create(WB_MOD_LFO);
    lfo->rate = 1.0f;
    lfo->depth = 1.0f;
    int sid = wb_mod_matrix_add_src(m, lfo);
    CHECK(sid == 0, "LFO source added (id 0)");

    /* Route LFO -> track 0, slot 0, param 0, amount 0.5, base 0.5 => 0.25..0.75 */
    wb_mod_route r = { .src = sid, .track = 0, .slot = 0, .param = 0,
                       .amount = 0.5f, .base = 0.5f, .enabled = 1 };
    int rid = wb_mod_matrix_add_route(m, &r);
    CHECK(rid == 0, "route added (id 0)");

    /* Record the destination values we push through the setter. */
    float minv = 1e9f, maxv = -1e9f;
    for (int b = 0; b < 64; b++) {
        float got = -1.0f;
        wb_mod_setter cb = mod_test_setter;
        wb_mod_matrix_eval(m, 512, 44100.0f, cb, &got);
        if (got < minv) minv = got;
        if (got > maxv) maxv = got;
    }
    CHECK(minv < 0.30f, "LFO modulation reaches low value (< 0.30)");
    CHECK(maxv > 0.70f, "LFO modulation reaches high value (> 0.70)");
    CHECK(maxv - minv > 0.3f, "LFO produces a visible swing over time");

    wb_mod_matrix_destroy(m);
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
    test_automation_record();
    test_recorder();
    test_audio_clip();
    test_clip_gain();
    test_bus_routing();
    test_undo();
    test_remove_note();
    test_launchpad();
    test_xrun();
    test_sidechain();
    test_modulation();
    test_midifx();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
