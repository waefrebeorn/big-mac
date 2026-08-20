/* test_tts.c — verify the legitimate in-repo TTS engine (R020).
 * Big Mac HOSTS Piper (VITS, Apache-2.0, offline) exactly like the caption
 * engine hosts whisper.cpp. Checks: non-empty PCM, finite, sane duration, no
 * NaN, determinism (two runs equal), backend is NEURAL (vendored Piper), rate
 * changes output, and WAV write. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "wbus/wbus_tts.h"

static int failures = 0, checks = 0;
#define CK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

static int has_nan(const float *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (isnan(b[i]) || isinf(b[i])) return 1;
    return 0;
}

int main(void) {
    printf("=== R020 TTS engine (Piper, hosted) ===\n\n");

    wb_tts *t = wb_tts_create(NULL);   /* vendored Piper neural backend */
    CK(t != NULL, "engine created");
    CK(wb_tts_get_backend(t) == WB_TTS_BACKEND_NEURAL, "backend is NEURAL (vendored Piper, offline)");
    CK(wb_tts_sample_rate(t) == 22050, "sample rate 22050");
    CK(wb_tts_voice_count(t) >= 1, "has >=1 voice");

    float *p1 = NULL; uint32_t n1 = 0; int sr1 = 0;
    int rc = wb_tts_speak(t, "Hello world, this is the Big Mac editor.", &p1, &n1, &sr1);
    CK(rc == 0 && p1 != NULL, "speak succeeded");
    CK(n1 > 1000, "produced a non-empty buffer");
    CK(sr1 == 22050, "reported sample rate matches");
    CK(!has_nan(p1, n1), "no NaN/Inf in output");
    double dur = (double)n1 / 22050.0;
    printf("  [dbg] duration=%.2fs frames=%u\n", dur, n1);
    CK(dur > 0.5 && dur < 30.0, "duration in sane range (0.5..30s)");

    /* peak within range */
    float pk = 0; for (uint32_t i = 0; i < n1; i++) if (fabsf(p1[i]) > pk) pk = fabsf(p1[i]);
    CK(pk > 0.01f && pk <= 1.0001f, "non-silent, not clipped");

    /* determinism: same input -> same output */
    float *p2 = NULL; uint32_t n2 = 0; int sr2 = 0;
    wb_tts_speak(t, "Hello world, this is the Big Mac editor.", &p2, &n2, &sr2);
    CK(n2 == n1, "deterministic frame count");
    int same = (n1 == n2);
    for (uint32_t i = 0; same && i < n1; i++)
        if (p1[i] != p2[i]) same = 0;
    CK(same, "deterministic samples (bit-identical)");

    /* rate control changes output (pitch is voice-driven, rate is honored) */
    wb_tts_set_rate(t, 1.5f);
    float *p3 = NULL; uint32_t n3 = 0; int sr3 = 0;
    wb_tts_speak(t, "Hello world, this is the Big Mac editor.", &p3, &n3, &sr3);
    int diff = (n3 != n1);
    CK(diff, "different rate -> different output (faster -> fewer frames)");
    wb_tts_set_rate(t, 1.0f);

    free(p1); free(p2); free(p3);

    /* WAV convenience */
    CK(wb_tts_speak_wav(t, "Big Mac can narrate offline.", "/tmp/test_tts.wav") == 0,
       "speak_wav wrote a file");

    wb_tts_destroy(t);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
