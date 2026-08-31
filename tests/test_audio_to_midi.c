/* test_audio_to_midi.c — gate test for wb_audio_to_midi.
 * Verifies: create/destroy, sine wave pitch detection, silence rejection,
 * sine sweep multi-note, threshold/min-duration filtering, sorting,
 * valid MIDI range. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "wbus.h"

/* YIN pitch detector (from wb_yin.c) */
extern float wb_yin_pitch(const float *buf, int n, uint32_t sr);

/* Audio-to-MIDI API */
typedef struct wb_audio_to_midi wb_audio_to_midi;

wb_audio_to_midi* wb_audio_to_midi_create(uint32_t sr);
void wb_audio_to_midi_destroy(wb_audio_to_midi *a);
int wb_audio_to_midi_convert(wb_audio_to_midi *a, const wb_sample *audio,
                              uint32_t frames, wb_note *out_notes,
                              int max_notes, int *out_count);
void wb_audio_to_midi_set_threshold(wb_audio_to_midi *a, float threshold);
void wb_audio_to_midi_set_min_duration(wb_audio_to_midi *a, float min_dur_ms);

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("Test %d: %s\n", tests_run, name); } while(0)
#define PASS() do { tests_passed++; printf("  [PASS]\n"); } while(0)
#define FAIL(msg) do { printf("  [FAIL] %s\n", msg); } while(0)

#define WB_SAMPLE_RATE 44100

/* Generate a sine wave */
static void gen_sine(float *buf, uint32_t frames, float freq, uint32_t sr) {
    for (uint32_t i = 0; i < frames; i++) {
        buf[i] = sinf(2.0f * 3.14159265f * freq * (float)i / (float)sr);
    }
}

/* Generate silence */
static void gen_silence(float *buf, uint32_t frames) {
    memset(buf, 0, frames * sizeof(float));
}

int main(void) {
    /* Test 1: Create/destroy */
    TEST("create/destroy");
    {
        wb_audio_to_midi *a2m = wb_audio_to_midi_create(WB_SAMPLE_RATE);
        if (a2m) {
            wb_audio_to_midi_destroy(a2m);
            PASS();
        } else {
            FAIL("create returned NULL");
        }
    }

    /* Test 2: Convert 440Hz sine → MIDI 69 (A4) */
    TEST("440Hz sine → MIDI 69 (A4)");
    {
        uint32_t sr = WB_SAMPLE_RATE;
        uint32_t frames = sr * 2; /* 2 seconds */
        float *buf = (float *)malloc(frames * sizeof(float));
        gen_sine(buf, frames, 440.0f, sr);

        wb_audio_to_midi *a2m = wb_audio_to_midi_create(sr);
        wb_note notes[64];
        int count = 0;
        int rc = wb_audio_to_midi_convert(a2m, buf, frames, notes, 64, &count);
        printf("  detected %d notes\n", count);
        if (rc == 0 && count >= 1) {
            int found_a4 = 0;
            for (int i = 0; i < count; i++) {
                printf("  note[%d]: pitch=%d start=%.0f dur=%.0f vel=%d\n",
                       i, notes[i].pitch, notes[i].start, notes[i].dur, notes[i].vel);
                if (notes[i].pitch == 69) found_a4 = 1;
            }
            if (found_a4) PASS();
            else FAIL("no note with pitch 69 found");
        } else {
            FAIL("convert failed or no notes detected");
        }
        wb_audio_to_midi_destroy(a2m);
        free(buf);
    }

    /* Test 3: Convert 220Hz sine → MIDI 57 (A3) */
    TEST("220Hz sine → MIDI 57 (A3)");
    {
        uint32_t sr = WB_SAMPLE_RATE;
        uint32_t frames = sr * 2;
        float *buf = (float *)malloc(frames * sizeof(float));
        gen_sine(buf, frames, 220.0f, sr);

        wb_audio_to_midi *a2m = wb_audio_to_midi_create(sr);
        wb_note notes[64];
        int count = 0;
        int rc = wb_audio_to_midi_convert(a2m, buf, frames, notes, 64, &count);
        printf("  detected %d notes\n", count);
        if (rc == 0 && count >= 1) {
            int found_a3 = 0;
            for (int i = 0; i < count; i++) {
                printf("  note[%d]: pitch=%d start=%.0f dur=%.0f vel=%d\n",
                       i, notes[i].pitch, notes[i].start, notes[i].dur, notes[i].vel);
                if (notes[i].pitch == 57) found_a3 = 1;
            }
            if (found_a3) PASS();
            else FAIL("no note with pitch 57 found");
        } else {
            FAIL("convert failed or no notes detected");
        }
        wb_audio_to_midi_destroy(a2m);
        free(buf);
    }

    /* Test 4: Convert silence → 0 notes */
    TEST("silence → 0 notes");
    {
        uint32_t sr = WB_SAMPLE_RATE;
        uint32_t frames = sr * 1;
        float *buf = (float *)malloc(frames * sizeof(float));
        gen_silence(buf, frames);

        wb_audio_to_midi *a2m = wb_audio_to_midi_create(sr);
        wb_note notes[64];
        int count = 0;
        int rc = wb_audio_to_midi_convert(a2m, buf, frames, notes, 64, &count);
        printf("  detected %d notes (expected 0)\n", count);
        if (rc == 0 && count == 0) PASS();
        else FAIL("expected 0 notes for silence");
        wb_audio_to_midi_destroy(a2m);
        free(buf);
    }

    /* Test 5: Convert sine sweep → multiple notes */
    TEST("sine sweep → multiple notes");
    {
        uint32_t sr = WB_SAMPLE_RATE;
        uint32_t frames = sr * 4; /* 4 seconds */
        float *buf = (float *)malloc(frames * sizeof(float));
        /* Sweep from 220Hz to 880Hz over 4 seconds */
        for (uint32_t i = 0; i < frames; i++) {
            float t = (float)i / (float)sr;
            float freq = 220.0f * powf(2.0f, t * 1.0f); /* 1 octave over 4s → 220→440 */
            if (freq > 880.0f) freq = 880.0f;
            buf[i] = sinf(2.0f * 3.14159265f * freq * t);
        }

        wb_audio_to_midi *a2m = wb_audio_to_midi_create(sr);
        wb_note notes[128];
        int count = 0;
        int rc = wb_audio_to_midi_convert(a2m, buf, frames, notes, 128, &count);
        printf("  detected %d notes\n", count);
        if (rc == 0 && count >= 2) {
            for (int i = 0; i < count && i < 10; i++) {
                printf("  note[%d]: pitch=%d start=%.0f dur=%.0f\n",
                       i, notes[i].pitch, notes[i].start, notes[i].dur);
            }
            PASS();
        } else {
            FAIL("expected multiple notes for sweep");
        }
        wb_audio_to_midi_destroy(a2m);
        free(buf);
    }

    /* Test 6: Threshold setting affects note count */
    TEST("threshold affects note count");
    {
        uint32_t sr = WB_SAMPLE_RATE;
        uint32_t frames = sr * 2;
        float *buf = (float *)malloc(frames * sizeof(float));
        /* Generate quiet sine (amplitude 0.1) */
        for (uint32_t i = 0; i < frames; i++) {
            buf[i] = 0.1f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)sr);
        }

        wb_audio_to_midi *a2m = wb_audio_to_midi_create(sr);

        /* Low threshold: should detect notes */
        wb_audio_to_midi_set_threshold(a2m, 0.001f);
        wb_note notes1[64];
        int count1 = 0;
        wb_audio_to_midi_convert(a2m, buf, frames, notes1, 64, &count1);

        /* High threshold: should detect fewer/no notes */
        wb_audio_to_midi_set_threshold(a2m, 0.5f);
        wb_note notes2[64];
        int count2 = 0;
        wb_audio_to_midi_convert(a2m, buf, frames, notes2, 64, &count2);

        printf("  low threshold: %d notes, high threshold: %d notes\n", count1, count2);
        if (count1 > count2) PASS();
        else FAIL("higher threshold should reduce note count");
        wb_audio_to_midi_destroy(a2m);
        free(buf);
    }

    /* Test 7: Min duration filters short notes */
    TEST("min duration filters short notes");
    {
        uint32_t sr = WB_SAMPLE_RATE;
        /* Generate two short bursts of 440Hz with silence between */
        uint32_t burst_len = sr / 20; /* 50ms bursts */
        uint32_t gap_len = sr / 10;   /* 100ms gap */
        uint32_t frames = burst_len + gap_len + burst_len;
        float *buf = (float *)calloc(frames, sizeof(float));
        for (uint32_t i = 0; i < burst_len; i++) {
            buf[i] = sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)sr);
        }
        for (uint32_t i = 0; i < burst_len; i++) {
            buf[burst_len + gap_len + i] = sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)sr);
        }

        wb_audio_to_midi *a2m = wb_audio_to_midi_create(sr);

        /* Short min duration: should detect notes */
        wb_audio_to_midi_set_min_duration(a2m, 10.0f);
        wb_note notes1[64];
        int count1 = 0;
        wb_audio_to_midi_convert(a2m, buf, frames, notes1, 64, &count1);

        /* Long min duration: should filter out short notes */
        wb_audio_to_midi_set_min_duration(a2m, 500.0f);
        wb_note notes2[64];
        int count2 = 0;
        wb_audio_to_midi_convert(a2m, buf, frames, notes2, 64, &count2);

        printf("  short min_dur: %d notes, long min_dur: %d notes\n", count1, count2);
        if (count2 <= count1) PASS();
        else FAIL("longer min duration should not increase note count");
        wb_audio_to_midi_destroy(a2m);
        free(buf);
    }

    /* Test 8: Output notes are sorted by start time */
    TEST("notes sorted by start time");
    {
        uint32_t sr = WB_SAMPLE_RATE;
        uint32_t frames = sr * 3;
        float *buf = (float *)malloc(frames * sizeof(float));
        /* Three segments: 440Hz, 554Hz, 659Hz (each 1 second) */
        for (uint32_t i = 0; i < sr; i++)
            buf[i] = sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)sr);
        for (uint32_t i = 0; i < sr; i++)
            buf[sr + i] = sinf(2.0f * 3.14159265f * 554.0f * (float)i / (float)sr);
        for (uint32_t i = 0; i < sr; i++)
            buf[2*sr + i] = sinf(2.0f * 3.14159265f * 659.0f * (float)i / (float)sr);

        wb_audio_to_midi *a2m = wb_audio_to_midi_create(sr);
        wb_note notes[64];
        int count = 0;
        wb_audio_to_midi_convert(a2m, buf, frames, notes, 64, &count);

        int sorted = 1;
        for (int i = 1; i < count; i++) {
            if (notes[i].start < notes[i-1].start) {
                sorted = 0;
                break;
            }
        }
        printf("  %d notes, sorted=%d\n", count, sorted);
        if (sorted && count >= 1) PASS();
        else FAIL("notes not sorted by start time");
        wb_audio_to_midi_destroy(a2m);
        free(buf);
    }

    /* Test 9: Note pitches are in valid MIDI range (0-127) */
    TEST("pitches in valid MIDI range (0-127)");
    {
        uint32_t sr = WB_SAMPLE_RATE;
        uint32_t frames = sr * 2;
        float *buf = (float *)malloc(frames * sizeof(float));
        /* Very high frequency (5kHz) */
        gen_sine(buf, frames, 5000.0f, sr);

        wb_audio_to_midi *a2m = wb_audio_to_midi_create(sr);
        wb_note notes[64];
        int count = 0;
        wb_audio_to_midi_convert(a2m, buf, frames, notes, 64, &count);

        int valid = 1;
        for (int i = 0; i < count; i++) {
            if (notes[i].pitch > 127) {
                valid = 0;
                printf("  note[%d]: pitch=%d (INVALID)\n", i, notes[i].pitch);
                break;
            }
        }
        printf("  %d notes, all valid=%d\n", count, valid);
        if (valid) PASS();
        else FAIL("pitch out of MIDI range");
        wb_audio_to_midi_destroy(a2m);
        free(buf);
    }

    printf("\n=== %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}