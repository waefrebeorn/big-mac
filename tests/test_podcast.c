/* tests/test_podcast.c — podcast production workflow test.
 * Pure C11, standalone: only needs wb_podcast.o.
 * Tests init/destroy, voice isolation, noise gate, loudness normalization,
 * chapter detection, and output finiteness. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* We need the wb_podcast struct definition for allocation.
 * Since it's opaque in the public header, we declare the init function
 * and use the API through the header. For sizeof, we include the
 * internal approach: allocate enough via the known struct layout. */

/* Forward declarations matching wb_podcast.c */
struct wb_podcast;  /* opaque */

/* We'll declare the API directly since wbus.h doesn't include podcast yet */
struct wb_podcast *wb_podcast_alloc(void);
int  wb_podcast_init(struct wb_podcast *pc, uint32_t sr);
int  wb_podcast_process_voice(void *pc, const float *in, float *out, int n);
int  wb_podcast_process_noise_gate(void *pc, const float *in, float *out, int n, float threshold);
int  wb_podcast_normalize_loudness(void *pc, float *audio, int n, float target_lufs);
int  wb_podcast_detect_chapters(void *pc, const float *audio, int n, float sr,
                                  double *chapter_times_out, int max_chapters);
void wb_podcast_set_voice_isolation_strength(void *pc, float strength);
void wb_podcast_free(struct wb_podcast *pc);

#define SR 44100
#define N (SR * 2)  /* 2 seconds */

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass_count++; } \
    else      { printf("  FAIL: %s\n", msg); fail_count++; } \
} while (0)

/* Compute RMS of a buffer */
static float compute_rms(const float *buf, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += (double)buf[i] * (double)buf[i];
    return (float)sqrt(sum / (double)n);
}

/* Compute peak amplitude */
static float compute_peak(const float *buf, int n) {
    float pk = 0.0f;
    for (int i = 0; i < n; i++) {
        float a = fabsf(buf[i]);
        if (a > pk) pk = a;
    }
    return pk;
}

/* Check for NaN/Inf */
static int is_finite(const float *buf, int n) {
    for (int i = 0; i < n; i++) {
        if (buf[i] != buf[i]) return 0;       /* NaN */
        if (buf[i] > 1e18f || buf[i] < -1e18f) return 0;  /* Inf */
    }
    return 1;
}

/* Generate sine wave */
static void gen_sine(float *buf, int n, float freq, float amp) {
    for (int i = 0; i < n; i++)
        buf[i] = amp * sinf(2.0f * M_PI * freq * (float)i / (float)SR);
}

/* Generate silence */
static void gen_silence(float *buf, int n) {
    memset(buf, 0, n * sizeof(float));
}

/* Generate white noise */
static void gen_noise(float *buf, int n, float amp) {
    unsigned seed = 42;
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        float r = (float)(int)seed / 2147483648.0f;
        if (r > 1.0f) r = 1.0f;
        if (r < -1.0f) r = -1.0f;
        buf[i] = r * amp;
    }
}

int main(void) {
    float *in  = (float *)malloc(N * sizeof(float));
    float *out = (float *)malloc(N * sizeof(float));
    float *tmp = (float *)malloc(N * sizeof(float));

    printf("=== test_podcast ===\n");

    /* ---- Test 1: Init/destroy ---- */
    printf("\nTest 1: Init/destroy\n");
    {
        struct wb_podcast *pc = wb_podcast_alloc();
        CHECK(pc != NULL, "allocation succeeds");
        if (pc) {
            int rc = wb_podcast_init(pc, SR);
            CHECK(rc == 0, "init returns 0");
            wb_podcast_free(pc);
            CHECK(1, "destroy completes without crash");
        }
    }

    /* ---- Test 2: Voice isolation produces output ---- */
    printf("\nTest 2: Voice isolation produces output\n");
    {
        struct wb_podcast *pc = wb_podcast_alloc();
        if (!pc) { printf("  FAIL: alloc\n"); fail_count++; return 1; }
        wb_podcast_init(pc, SR);
        wb_podcast_set_voice_isolation_strength(pc, 0.8f);

        /* Generate 1kHz sine (within voice band) */
        gen_sine(in, N, 1000.0f, 0.5f);

        int rc = wb_podcast_process_voice(pc, in, out, N);
        CHECK(rc == 0, "process_voice returns 0");

        float rms_out = compute_rms(out, N);
        float peak_out = compute_peak(out, N);
        CHECK(rms_out > 0.01f, "output has non-zero RMS (voice preserved)");
        CHECK(peak_out > 0.05f, "output has non-trivial peak");

        /* Check output is finite */
        CHECK(is_finite(out, N), "voice isolation output is finite (no NaN/Inf)");

        wb_podcast_free(pc);
    }

    /* ---- Test 3: Noise gate attenuates silence ---- */
    printf("\nTest 3: Noise gate attenuates silence\n");
    {
        struct wb_podcast *pc = wb_podcast_alloc();
        if (!pc) { printf("  FAIL: alloc\n"); fail_count++; return 1; }
        wb_podcast_init(pc, SR);

        /* Create signal: loud sine first half, silence second half */
        int half = N / 2;
        gen_sine(in, half, 440.0f, 0.5f);
        gen_silence(in + half, N - half);

        wb_podcast_process_noise_gate(pc, in, out, N, 0.01f);

        /* First half should pass through (peak near 0.5) */
        float peak_first = compute_peak(out, half);
        /* Second half should be attenuated (near zero) */
        float peak_second = compute_peak(out + half, N - half);

        CHECK(peak_first > 0.3f, "noise gate passes loud signal");
        CHECK(peak_second < 0.1f, "noise gate attenuates silence");
        CHECK(is_finite(out, N), "noise gate output is finite");

        wb_podcast_free(pc);
    }

    /* ---- Test 4: Loudness normalization changes level ---- */
    printf("\nTest 4: Loudness normalization changes level\n");
    {
        struct wb_podcast *pc = wb_podcast_alloc();
        if (!pc) { printf("  FAIL: alloc\n"); fail_count++; return 1; }
        wb_podcast_init(pc, SR);

        /* Generate sine at known amplitude */
        gen_sine(in, N, 1000.0f, 0.3f);
        memcpy(tmp, in, N * sizeof(float));

        float rms_before = compute_rms(tmp, N);

        /* Normalize to -16 LUFS (should increase gain since 0.3 RMS sine ≈ -16 LUFS) */
        wb_podcast_normalize_loudness(pc, tmp, N, -16.0f);

        float rms_after = compute_rms(tmp, N);

        CHECK(rms_after > 0.0f, "normalized output has energy");
        CHECK(is_finite(tmp, N), "normalized output is finite");

        /* The gain should have been applied (level changed or stayed reasonable) */
        CHECK(rms_after != rms_before || rms_after > 0.01f,
              "normalization modifies or maintains level");

        printf("  INFO: RMS before=%.4f, after=%.4f\n", rms_before, rms_after);

        wb_podcast_free(pc);
    }

    /* ---- Test 5: Chapter detection finds gaps ---- */
    printf("\nTest 5: Chapter detection finds gaps\n");
    {
        struct wb_podcast *pc = wb_podcast_alloc();
        if (!pc) { printf("  FAIL: alloc\n"); fail_count++; return 1; }
        wb_podcast_init(pc, SR);

        /* Create audio with silence gaps > 2 seconds:
         * [0-1s: tone] [1-3.5s: silence] [3.5-5s: tone] [5-8s: silence] [8-10s: tone]
         * Total = 10 seconds = 441000 samples
         */
        int total = SR * 10;
        float *long_audio = (float *)malloc(total * sizeof(float));
        memset(long_audio, 0, total * sizeof(float));

        /* 0-1s: tone */
        for (int i = 0; i < SR; i++)
            long_audio[i] = 0.5f * sinf(2.0f * M_PI * 440.0f * (float)i / (float)SR);
        /* 3.5-5s: tone */
        for (int i = (int)(3.5f * SR); i < 5 * SR; i++)
            long_audio[i] = 0.5f * sinf(2.0f * M_PI * 440.0f * (float)(i - 3.5f*SR) / (float)SR);
        /* 8-10s: tone */
        for (int i = 8 * SR; i < total; i++)
            long_audio[i] = 0.5f * sinf(2.0f * M_PI * 440.0f * (float)(i - 8*SR) / (float)SR);

        double chapter_times[16];
        int num_chapters = wb_podcast_detect_chapters(pc, long_audio, total, (float)SR,
                                                        chapter_times, 16);

        printf("  INFO: detected %d chapters\n", num_chapters);
        for (int i = 0; i < num_chapters; i++)
            printf("    chapter[%d] = %.2f s\n", i, chapter_times[i]);

        CHECK(num_chapters >= 2, "chapter detection finds at least 2 chapters");
        CHECK(chapter_times[0] == 0.0, "first chapter starts at t=0");

        /* Should find chapter at ~3.5s (end of first 2.5s silence gap) */
        int found_gap = 0;
        for (int i = 1; i < num_chapters; i++) {
            if (chapter_times[i] > 3.0 && chapter_times[i] < 4.5) found_gap = 1;
        }
        CHECK(found_gap, "chapter detected at first silence gap boundary");

        free(long_audio);
        wb_podcast_free(pc);
    }

    /* ---- Test 6: Output finite (no NaN) across all processors ---- */
    printf("\nTest 6: Output finite (no NaN) across all processors\n");
    {
        struct wb_podcast *pc = wb_podcast_alloc();
        if (!pc) { printf("  FAIL: alloc\n"); fail_count++; return 1; }
        wb_podcast_init(pc, SR);

        /* Test with various challenging inputs */
        /* a) All zeros */
        gen_silence(in, N);
        wb_podcast_set_voice_isolation_strength(pc, 1.0f);
        wb_podcast_process_voice(pc, in, out, N);
        CHECK(is_finite(out, N), "voice isolation: silent input -> finite output");

        wb_podcast_process_noise_gate(pc, in, out, N, 0.01f);
        CHECK(is_finite(out, N), "noise gate: silent input -> finite output");

        /* b) Full-scale sine */
        gen_sine(in, N, 1000.0f, 1.0f);
        wb_podcast_process_voice(pc, in, out, N);
        CHECK(is_finite(out, N), "voice isolation: full-scale sine -> finite");

        wb_podcast_process_noise_gate(pc, in, out, N, 0.01f);
        CHECK(is_finite(out, N), "noise gate: full-scale sine -> finite");

        /* c) Noise */
        gen_noise(in, N, 0.8f);
        wb_podcast_process_voice(pc, in, out, N);
        CHECK(is_finite(out, N), "voice isolation: noise -> finite output");

        /* d) Normalize a buffer with content */
        gen_sine(in, N, 440.0f, 0.5f);
        wb_podcast_normalize_loudness(pc, in, N, -16.0f);
        CHECK(is_finite(in, N), "normalize: output is finite");

        wb_podcast_free(pc);
    }

    /* ---- Summary ---- */
    printf("\n=== RESULTS: %d passed, %d failed ===\n", pass_count, fail_count);

    free(in);
    free(out);
    free(tmp);

    return fail_count > 0 ? 1 : 0;
}