/* tests/test_surround_mix.c — 5.1 surround audio mixing verification.
 *
 * Tests wb_audio_mix_surround() to verify correct gain distribution
 * across the 6 output channels (L, R, C, LFE, Ls, Rs) based on
 * per-clip pan position.
 *
 * Test strategy:
 *   - Create a WAV file with a known signal (sine wave)
 *   - Add it as an audio clip with a specific pan position
 *   - Call wb_audio_mix_surround()
 *   - Verify each channel gets the expected signal level
 */

#include "wbus/wbus_edit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 48000
#define TEST_FREQ 440.0
#define TEST_DURATION 0.1  /* seconds */
#define N_FRAMES (int)(SAMPLE_RATE * TEST_DURATION)
#define EPSILON 0.01f

/* Write a mono 16-bit PCM WAV file with a sine wave */
static int write_test_wav(const char *path, double freq, double duration) {
    int n_frames = (int)(SAMPLE_RATE * duration);
    int16_t *pcm = (int16_t *)malloc(n_frames * sizeof(int16_t));
    if (!pcm) return -1;

    for (int i = 0; i < n_frames; i++) {
        double t = (double)i / SAMPLE_RATE;
        double sample = 0.5 * sin(2.0 * M_PI * freq * t);  /* 0.5 amplitude */
        pcm[i] = (int16_t)(sample * 32767.0);
    }

    FILE *f = fopen(path, "wb");
    if (!f) { free(pcm); return -1; }

    /* WAV header */
    uint32_t data_size = n_frames * 2;
    uint32_t file_size = 36 + data_size;
    uint32_t byte_rate = SAMPLE_RATE * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;

    fwrite("RIFF", 1, 4, f);
    uint32_t v = file_size; fwrite(&v, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    v = 16; fwrite(&v, 4, 1, f);
    uint16_t fmt = 1; fwrite(&fmt, 2, 1, f);
    uint16_t channels = 1; fwrite(&channels, 2, 1, f);
    v = SAMPLE_RATE; fwrite(&v, 4, 1, f);
    v = byte_rate; fwrite(&v, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    fwrite("data", 1, 4, f);
    v = data_size; fwrite(&v, 4, 1, f);
    fwrite(pcm, 1, data_size, f);
    fclose(f);
    free(pcm);
    return 0;
}

/* Compute RMS of a float buffer */
static float compute_rms(const float *buf, int n) {
    if (n <= 0) return 0.0f;
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += (double)buf[i] * (double)buf[i];
    }
    return (float)sqrt(sum / n);
}

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  PASS: %s\n", msg); \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

#define CHECK_NEAR(a, b, tol, msg) do { \
    tests_run++; \
    if (fabsf((a) - (b)) < (tol)) { \
        tests_passed++; \
        printf("  PASS: %s (got %.4f, expected %.4f)\n", msg, (float)(a), (float)(b)); \
    } else { \
        printf("  FAIL: %s (got %.4f, expected %.4f, tol %.4f)\n", msg, (float)(a), (float)(b), (float)(tol)); \
    } \
} while (0)

#define CHECK_GREATER(a, b, msg) do { \
    tests_run++; \
    if ((a) > (b)) { \
        tests_passed++; \
        printf("  PASS: %s (%.4f > %.4f)\n", msg, (float)(a), (float)(b)); \
    } else { \
        printf("  FAIL: %s (%.4f <= %.4f)\n", msg, (float)(a), (float)(b)); \
    } \
} while (0)

#define CHECK_LESS(a, b, msg) do { \
    tests_run++; \
    if ((a) < (b)) { \
        tests_passed++; \
        printf("  PASS: %s (%.4f < %.4f)\n", msg, (float)(a), (float)(b)); \
    } else { \
        printf("  FAIL: %s (%.4f >= %.4f)\n", msg, (float)(a), (float)(b)); \
    } \
} while (0)

int main(void) {
    printf("=== 5.1 Surround Mix Tests ===\n\n");

    const char *wav_path = "/tmp/test_surround_sine.wav";
    if (write_test_wav(wav_path, TEST_FREQ, TEST_DURATION) != 0) {
        printf("ERROR: Could not create test WAV file\n");
        return 1;
    }

    /* ---- Test 1: Center pan (0.0) -> signal goes to C channel ---- */
    printf("Test 1: Center pan (pan=0.0)\n");
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        int t = wb_edit_add_track(g, "Center Track");
        int ai = wb_edit_add_audio_clip(g, t, wav_path, 0.0, TEST_DURATION, 0.0);
        g->tracks[t].audio_clips[ai].pan = 0.0f;
        g->tracks[t].audio_clips[ai].volume = 1.0f;

        float *ch_L = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_R = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_C = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_LFE = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_Ls = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_Rs = (float *)calloc(N_FRAMES, sizeof(float));

        int contrib = wb_audio_mix_surround(g, ch_L, ch_R, ch_C, ch_LFE, ch_Ls, ch_Rs, 0.0, N_FRAMES);
        CHECK(contrib == 1, "one clip contributed to center pan mix");

        float rms_L = compute_rms(ch_L, N_FRAMES);
        float rms_R = compute_rms(ch_R, N_FRAMES);
        float rms_C = compute_rms(ch_C, N_FRAMES);
        float rms_LFE = compute_rms(ch_LFE, N_FRAMES);
        float rms_Ls = compute_rms(ch_Ls, N_FRAMES);
        float rms_Rs = compute_rms(ch_Rs, N_FRAMES);

        /* Center pan: C gets full signal, L/R/Ls/Rs get nothing */
        CHECK_LESS(rms_L, EPSILON, "L channel silent for center pan");
        CHECK_LESS(rms_R, EPSILON, "R channel silent for center pan");
        CHECK_GREATER(rms_C, 0.1f, "C channel has signal for center pan");
        CHECK_LESS(rms_Ls, EPSILON, "Ls channel silent for center pan");
        CHECK_LESS(rms_Rs, EPSILON, "Rs channel silent for center pan");
        /* LFE should have some signal (low-passed version of the mix) */
        CHECK_GREATER(rms_LFE, 0.001f, "LFE channel has signal for center pan");

        free(ch_L); free(ch_R); free(ch_C); free(ch_LFE); free(ch_Ls); free(ch_Rs);
        wb_edit_graph_destroy(g);
    }

    /* ---- Test 2: Full left pan (-1.0) -> signal goes to L + Ls ---- */
    printf("\nTest 2: Full left pan (pan=-1.0)\n");
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        int t = wb_edit_add_track(g, "Left Track");
        int ai = wb_edit_add_audio_clip(g, t, wav_path, 0.0, TEST_DURATION, 0.0);
        g->tracks[t].audio_clips[ai].pan = -1.0f;
        g->tracks[t].audio_clips[ai].volume = 1.0f;

        float *ch_L = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_R = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_C = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_LFE = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_Ls = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_Rs = (float *)calloc(N_FRAMES, sizeof(float));

        int contrib = wb_audio_mix_surround(g, ch_L, ch_R, ch_C, ch_LFE, ch_Ls, ch_Rs, 0.0, N_FRAMES);
        CHECK(contrib == 1, "one clip contributed to left pan mix");

        float rms_L = compute_rms(ch_L, N_FRAMES);
        float rms_R = compute_rms(ch_R, N_FRAMES);
        float rms_C = compute_rms(ch_C, N_FRAMES);
        float rms_Ls = compute_rms(ch_Ls, N_FRAMES);
        float rms_Rs = compute_rms(ch_Rs, N_FRAMES);

        /* Full left: L and Ls get signal, R/Rs/C get nothing */
        CHECK_GREATER(rms_L, 0.1f, "L channel has signal for full left pan");
        CHECK_LESS(rms_R, EPSILON, "R channel silent for full left pan");
        CHECK_LESS(rms_C, EPSILON, "C channel silent for full left pan");
        CHECK_GREATER(rms_Ls, 0.1f, "Ls channel has signal for full left pan");
        CHECK_LESS(rms_Rs, EPSILON, "Rs channel silent for full left pan");
        /* L and Ls should have similar levels (both full gain at pan=-1) */
        CHECK_NEAR(rms_L, rms_Ls, 0.05f, "L and Ls have similar levels at full left");

        free(ch_L); free(ch_R); free(ch_C); free(ch_LFE); free(ch_Ls); free(ch_Rs);
        wb_edit_graph_destroy(g);
    }

    /* ---- Test 3: Full right pan (+1.0) -> signal goes to R + Rs ---- */
    printf("\nTest 3: Full right pan (pan=+1.0)\n");
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        int t = wb_edit_add_track(g, "Right Track");
        int ai = wb_edit_add_audio_clip(g, t, wav_path, 0.0, TEST_DURATION, 0.0);
        g->tracks[t].audio_clips[ai].pan = 1.0f;
        g->tracks[t].audio_clips[ai].volume = 1.0f;

        float *ch_L = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_R = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_C = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_LFE = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_Ls = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_Rs = (float *)calloc(N_FRAMES, sizeof(float));

        int contrib = wb_audio_mix_surround(g, ch_L, ch_R, ch_C, ch_LFE, ch_Ls, ch_Rs, 0.0, N_FRAMES);
        CHECK(contrib == 1, "one clip contributed to right pan mix");

        float rms_L = compute_rms(ch_L, N_FRAMES);
        float rms_R = compute_rms(ch_R, N_FRAMES);
        float rms_C = compute_rms(ch_C, N_FRAMES);
        float rms_Ls = compute_rms(ch_Ls, N_FRAMES);
        float rms_Rs = compute_rms(ch_Rs, N_FRAMES);

        /* Full right: R and Rs get signal, L/Ls/C get nothing */
        CHECK_LESS(rms_L, EPSILON, "L channel silent for full right pan");
        CHECK_GREATER(rms_R, 0.1f, "R channel has signal for full right pan");
        CHECK_LESS(rms_C, EPSILON, "C channel silent for full right pan");
        CHECK_LESS(rms_Ls, EPSILON, "Ls channel silent for full right pan");
        CHECK_GREATER(rms_Rs, 0.1f, "Rs channel has signal for full right pan");
        /* R and Rs should have similar levels */
        CHECK_NEAR(rms_R, rms_Rs, 0.05f, "R and Rs have similar levels at full right");

        free(ch_L); free(ch_R); free(ch_C); free(ch_LFE); free(ch_Ls); free(ch_Rs);
        wb_edit_graph_destroy(g);
    }

    /* ---- Test 4: Half-left pan (-0.5) -> L full, Ls partial, C partial ---- */
    printf("\nTest 4: Half-left pan (pan=-0.5)\n");
    {
        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        int t = wb_edit_add_track(g, "Half-Left Track");
        int ai = wb_edit_add_audio_clip(g, t, wav_path, 0.0, TEST_DURATION, 0.0);
        g->tracks[t].audio_clips[ai].pan = -0.5f;
        g->tracks[t].audio_clips[ai].volume = 1.0f;

        float *ch_L = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_R = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_C = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_LFE = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_Ls = (float *)calloc(N_FRAMES, sizeof(float));
        float *ch_Rs = (float *)calloc(N_FRAMES, sizeof(float));

        int contrib = wb_audio_mix_surround(g, ch_L, ch_R, ch_C, ch_LFE, ch_Ls, ch_Rs, 0.0, N_FRAMES);
        CHECK(contrib == 1, "one clip contributed to half-left pan mix");

        float rms_L = compute_rms(ch_L, N_FRAMES);
        float rms_R = compute_rms(ch_R, N_FRAMES);
        float rms_C = compute_rms(ch_C, N_FRAMES);
        float rms_Ls = compute_rms(ch_Ls, N_FRAMES);
        float rms_Rs = compute_rms(ch_Rs, N_FRAMES);

        /* Half-left: L=full, Ls=0.5, C=0.5, R=0, Rs=0 */
        CHECK_GREATER(rms_L, 0.1f, "L channel has signal for half-left pan");
        CHECK_LESS(rms_R, EPSILON, "R channel silent for half-left pan");
        CHECK_GREATER(rms_C, 0.05f, "C channel has partial signal for half-left pan");
        CHECK_GREATER(rms_Ls, 0.05f, "Ls channel has partial signal for half-left pan");
        CHECK_LESS(rms_Rs, EPSILON, "Rs channel silent for half-left pan");
        /* Ls and C should be roughly equal (both at 0.5 gain) and less than L */
        CHECK_GREATER(rms_L, rms_Ls, "L > Ls at half-left pan");
        CHECK_GREATER(rms_L, rms_C, "L > C at half-left pan");
        CHECK_NEAR(rms_Ls, rms_C, 0.05f, "Ls ~= C at half-left pan");

        free(ch_L); free(ch_R); free(ch_C); free(ch_LFE); free(ch_Ls); free(ch_Rs);
        wb_edit_graph_destroy(g);
    }

    /* ---- Test 5: wb_audio_mixer_set_channels() ---- */
    printf("\nTest 5: wb_audio_mixer_set_channels()\n");
    {
        wb_audio_mixer_init();

        /* Default is stereo */
        CHECK(wb_audio_mixer_is_surround() == 0, "default is stereo (not surround)");

        /* Set to 5.1 */
        int rc = wb_audio_mixer_set_channels(6);
        CHECK(rc == 0, "set_channels(6) succeeds");
        CHECK(wb_audio_mixer_is_surround() == 1, "surround active after set_channels(6)");

        /* Set back to stereo */
        rc = wb_audio_mixer_set_channels(2);
        CHECK(rc == 0, "set_channels(2) succeeds");
        CHECK(wb_audio_mixer_is_surround() == 0, "stereo active after set_channels(2)");

        /* Invalid channel count */
        rc = wb_audio_mixer_set_channels(4);
        CHECK(rc == -1, "set_channels(4) returns -1 (invalid)");

        rc = wb_audio_mixer_set_channels(8);
        CHECK(rc == -1, "set_channels(8) returns -1 (invalid)");
    }

    /* ---- Test 6: NULL/invalid inputs ---- */
    printf("\nTest 6: NULL/invalid inputs\n");
    {
        float buf[100];
        memset(buf, 0, sizeof(buf));

        int rc = wb_audio_mix_surround(NULL, buf, buf, buf, buf, buf, buf, 0.0, 100);
        CHECK(rc == 0, "NULL graph returns 0 contributions");

        wb_edit_graph *g = wb_edit_graph_create(30.0, 854, 480);
        rc = wb_audio_mix_surround(g, NULL, buf, buf, buf, buf, buf, 0.0, 100);
        CHECK(rc == 0, "NULL ch_L returns 0 contributions");

        rc = wb_audio_mix_surround(g, buf, buf, buf, buf, buf, buf, 0.0, 0);
        CHECK(rc == 0, "zero frames returns 0 contributions");

        rc = wb_audio_mix_surround(g, buf, buf, buf, buf, buf, buf, 0.0, -1);
        CHECK(rc == 0, "negative frames returns 0 contributions");

        wb_edit_graph_destroy(g);
    }

    /* ---- Summary ---- */
    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);

    /* Cleanup */
    remove(wav_path);

    return (tests_passed == tests_run) ? 0 : 1;
}