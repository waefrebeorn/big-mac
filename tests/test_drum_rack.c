/* test_drum_rack.c — gate test for wb_drum_rack (pad sampler).
 * Verifies: create/destroy, load/trigger pads, volume/pan/mute/solo, clear. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "wbus.h"

/* Drum rack API declarations. */
wb_drum_rack *wb_drum_rack_create(uint32_t sr);
void  wb_drum_rack_destroy(wb_drum_rack *r);
int   wb_drum_rack_load_pad(wb_drum_rack *r, int pad_index,
                            const wb_sample *audio, uint32_t frames,
                            uint32_t channels);
int   wb_drum_rack_load_pad_file(wb_drum_rack *r, int pad_index,
                                 const char *path);
int   wb_drum_rack_trigger(wb_drum_rack *r, int pad_index, float velocity);
int   wb_drum_rack_trigger_note(wb_drum_rack *r, int midi_note, float velocity);
void  wb_drum_rack_process(wb_drum_rack *r, wb_sample *out, uint32_t frames);
void  wb_drum_rack_set_pad_volume(wb_drum_rack *r, int pad, float vol);
void  wb_drum_rack_set_pad_pan(wb_drum_rack *r, int pad, float pan);
void  wb_drum_rack_set_pad_mute(wb_drum_rack *r, int pad, int mute);
void  wb_drum_rack_set_pad_solo(wb_drum_rack *r, int pad, int solo);
void  wb_drum_rack_set_master_volume(wb_drum_rack *r, float vol);
void  wb_drum_rack_clear(wb_drum_rack *r);
int   wb_drum_rack_pad_count(wb_drum_rack *r);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

/* Compute RMS of interleaved stereo buffer. */
static float rms_stereo(const wb_sample *buf, uint32_t frames) {
    float sum = 0;
    for (uint32_t i = 0; i < frames * 2; i++) sum += buf[i] * buf[i];
    return sqrtf(sum / (float)(frames * 2));
}

/* Compute RMS of left channel only. */
static float rms_left(const wb_sample *buf, uint32_t frames) {
    float sum = 0;
    for (uint32_t i = 0; i < frames; i++) sum += buf[i * 2] * buf[i * 2];
    return sqrtf(sum / (float)frames);
}

/* Compute RMS of right channel only. */
static float rms_right(const wb_sample *buf, uint32_t frames) {
    float sum = 0;
    for (uint32_t i = 0; i < frames; i++) sum += buf[i * 2 + 1] * buf[i * 2 + 1];
    return sqrtf(sum / (float)frames);
}

/* Synthesize a simple kick-like sample (sine sweep + decay). */
static wb_sample *make_kick(uint32_t frames, uint32_t *out_frames) {
    wb_sample *s = (wb_sample *)calloc(frames * 2, sizeof(wb_sample));
    if (!s) return NULL;
    for (uint32_t i = 0; i < frames; i++) {
        float t = (float)i / 44100.0f;
        float env = expf(-t * 8.0f);
        float freq = 150.0f * expf(-t * 20.0f) + 50.0f;
        float phase = 2.0f * 3.14159265f * freq * t;
        float v = sinf(phase) * env * 0.8f;
        s[i * 2] = v;
        s[i * 2 + 1] = v;
    }
    *out_frames = frames;
    return s;
}

/* Synthesize a simple snare-like sample (noise burst + decay). */
static wb_sample *make_snare(uint32_t frames, uint32_t *out_frames) {
    wb_sample *s = (wb_sample *)calloc(frames * 2, sizeof(wb_sample));
    if (!s) return NULL;
    unsigned int rng = 0xDEADBEEFu;
    for (uint32_t i = 0; i < frames; i++) {
        float t = (float)i / 44100.0f;
        float env = expf(-t * 15.0f);
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        float noise = (float)((int)(rng & 0xFFFF) - 32768) / 32768.0f;
        float v = noise * env * 0.6f;
        s[i * 2] = v;
        s[i * 2 + 1] = v;
    }
    *out_frames = frames;
    return s;
}

#define BLOCK 44100

int main(void) {
    wb_sample out[BLOCK * 2];

    /* Test 1: create/destroy */
    TEST("create/destroy");
    {
        wb_drum_rack *r = wb_drum_rack_create(44100);
        if (r) { wb_drum_rack_destroy(r); PASS(); }
        else FAIL("create returned NULL");
    }

    wb_drum_rack *r = wb_drum_rack_create(44100);
    if (!r) { printf("FATAL: cannot create drum rack\n"); return 1; }

    /* Test 2: load synthesized kick onto pad 0 */
    TEST("load kick onto pad 0");
    {
        uint32_t frames;
        wb_sample *kick = make_kick(22050, &frames);
        if (!kick) { FAIL("alloc kick"); }
        else {
            int rc = wb_drum_rack_load_pad(r, 0, kick, frames, 2);
            if (rc == 0) PASS(); else FAIL("load_pad failed");
            free(kick);
        }
    }

    /* Test 3: trigger pad, verify audio output is non-silent */
    TEST("trigger pad 0, non-silent output");
    {
        memset(out, 0, sizeof(out));
        wb_drum_rack_trigger(r, 0, 1.0f);
        wb_drum_rack_process(r, out, BLOCK);
        float rms = rms_stereo(out, BLOCK);
        printf("  RMS = %.6f\n", rms);
        if (rms > 0.001f) PASS(); else FAIL("output silent");
    }

    /* Test 4: trigger with different velocities, verify amplitude scales */
    TEST("velocity scaling");
    {
        memset(out, 0, sizeof(out));
        wb_drum_rack_clear(r);
        wb_drum_rack_trigger(r, 0, 0.25f);
        wb_drum_rack_process(r, out, BLOCK);
        float rms_low = rms_stereo(out, BLOCK);

        memset(out, 0, sizeof(out));
        wb_drum_rack_clear(r);
        wb_drum_rack_trigger(r, 0, 1.0f);
        wb_drum_rack_process(r, out, BLOCK);
        float rms_high = rms_stereo(out, BLOCK);

        printf("  RMS low vel = %.6f, high vel = %.6f\n", rms_low, rms_high);
        if (rms_high > rms_low * 2.0f && rms_low > 0.0001f)
            PASS();
        else
            FAIL("velocity does not scale amplitude");
    }

    /* Test 5: set pad volume, verify output level changes */
    TEST("pad volume changes output level");
    {
        wb_drum_rack_clear(r);
        wb_drum_rack_set_pad_volume(r, 0, 0.2f);
        memset(out, 0, sizeof(out));
        wb_drum_rack_trigger(r, 0, 1.0f);
        wb_drum_rack_process(r, out, BLOCK);
        float rms_low = rms_stereo(out, BLOCK);

        wb_drum_rack_clear(r);
        wb_drum_rack_set_pad_volume(r, 0, 1.0f);
        memset(out, 0, sizeof(out));
        wb_drum_rack_trigger(r, 0, 1.0f);
        wb_drum_rack_process(r, out, BLOCK);
        float rms_high = rms_stereo(out, BLOCK);

        printf("  RMS vol=0.2: %.6f, vol=1.0: %.6f\n", rms_low, rms_high);
        wb_drum_rack_set_pad_volume(r, 0, 1.0f); /* restore */
        if (rms_high > rms_low * 2.0f) PASS(); else FAIL("volume not effective");
    }

    /* Test 6: set pad pan, verify stereo output differs */
    TEST("pad pan affects stereo");
    {
        wb_drum_rack_clear(r);
        wb_drum_rack_set_pad_pan(r, 0, -1.0f); /* full left */
        memset(out, 0, sizeof(out));
        wb_drum_rack_trigger(r, 0, 1.0f);
        wb_drum_rack_process(r, out, BLOCK);
        float left_rms = rms_left(out, BLOCK);
        float right_rms = rms_right(out, BLOCK);

        printf("  pan=L: left RMS = %.6f, right RMS = %.6f\n", left_rms, right_rms);
        if (left_rms > right_rms * 3.0f && left_rms > 0.001f)
            PASS();
        else
            FAIL("pan left did not favor left channel");

        wb_drum_rack_set_pad_pan(r, 0, 0.0f); /* restore center */
    }

    /* Test 7: mute pad, verify silence */
    TEST("mute pad produces silence");
    {
        wb_drum_rack_clear(r);
        wb_drum_rack_set_pad_mute(r, 0, 1);
        memset(out, 0, sizeof(out));
        int rc = wb_drum_rack_trigger(r, 0, 1.0f);
        wb_drum_rack_process(r, out, BLOCK);
        float rms = rms_stereo(out, BLOCK);
        printf("  trigger returned %d, RMS = %.6e\n", rc, rms);
        wb_drum_rack_set_pad_mute(r, 0, 0); /* restore */
        if (rc < 0 && rms < 0.0001f) PASS(); else FAIL("mute did not silence");
    }

    /* Test 8: solo pad, verify only soloed pad plays */
    TEST("solo: only soloed pad plays");
    {
        /* Load snare onto pad 1. */
        uint32_t snare_frames;
        wb_sample *snare = make_snare(16000, &snare_frames);
        if (!snare) { FAIL("alloc snare"); }
        else {
            wb_drum_rack_load_pad(r, 1, snare, snare_frames, 2);
            free(snare);
        }

        /* Solo pad 0 only — trigger pad 1 (snare), should be silent. */
        wb_drum_rack_clear(r);
        wb_drum_rack_set_pad_solo(r, 0, 1);
        memset(out, 0, sizeof(out));
        (void)wb_drum_rack_trigger(r, 1, 1.0f); /* snare should be gated */
        wb_drum_rack_process(r, out, BLOCK);
        float rms_snare_soloed = rms_stereo(out, BLOCK);

        /* Now trigger pad 0 (soloed), should be audible. */
        memset(out, 0, sizeof(out));
        wb_drum_rack_trigger(r, 0, 1.0f);
        wb_drum_rack_process(r, out, BLOCK);
        float rms_kick_soloed = rms_stereo(out, BLOCK);

        printf("  snare (not soloed) RMS = %.6e, kick (soloed) RMS = %.6f\n",
               rms_snare_soloed, rms_kick_soloed);

        wb_drum_rack_set_pad_solo(r, 0, 0); /* restore */
        if (rms_snare_soloed < 0.0001f && rms_kick_soloed > 0.001f)
            PASS();
        else
            FAIL("solo logic incorrect");
    }

    /* Test 9: clear stops all voices */
    TEST("clear stops all voices");
    {
        wb_drum_rack_trigger(r, 0, 1.0f);
        wb_drum_rack_trigger(r, 1, 1.0f);
        wb_drum_rack_clear(r);
        memset(out, 0, sizeof(out));
        wb_drum_rack_process(r, out, BLOCK);
        float rms = rms_stereo(out, BLOCK);
        printf("  RMS after clear = %.6e\n", rms);
        if (rms < 0.0001f) PASS(); else FAIL("voices still playing");
    }

    /* Test 10: load multiple pads, trigger simultaneously */
    TEST("multiple pads simultaneous");
    {
        wb_drum_rack_clear(r);
        memset(out, 0, sizeof(out));
        wb_drum_rack_trigger(r, 0, 1.0f);
        wb_drum_rack_trigger(r, 1, 1.0f);
        wb_drum_rack_process(r, out, BLOCK);
        float rms = rms_stereo(out, BLOCK);
        printf("  RMS = %.6f\n", rms);
        if (rms > 0.005f) PASS(); else FAIL("multi-pad silent");
    }

    /* Test 11: pad count tracks loaded pads */
    TEST("pad count tracks loaded pads");
    {
        int count = wb_drum_rack_pad_count(r);
        printf("  pad_count = %d\n", count);
        if (count == 2) PASS(); else FAIL("pad count wrong");
    }

    wb_drum_rack_destroy(r);

    printf("\n=== RESULT: %d/%d checks passed, %d failures ===\n",
           tests_passed, tests_run, tests_run - tests_passed);
    return tests_passed == tests_run ? 0 : 1;
}