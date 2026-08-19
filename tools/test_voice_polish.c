/* test_voice_polish.c — headless verification of the voice-polish preset
 * chain (R015 Tier 1): gate -> deesser -> comp -> EQ -> limiter -> loudness. */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "wbus/wbus_voice_polish.h"
#include "wbus/wbus_dsp.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

/* simple speech-ish signal: vowel formant carrier + occasional sibilance burst */
static void make_signal(float *b, int n, int ch) {
    for (int i = 0; i < n; i++) {
        float t = (float)i / 44100.0f;
        /* base voice: 150 Hz sawtooth-ish + 1 kHz formant */
        float v = 0.4f * (float)sinf(2*M_PI*150*t)
                + 0.25f * (float)sinf(2*M_PI*1000*t);
        /* sibilance: every 0.5 s for 40 ms, strong 6 kHz */
        int phase = i % 22050;
        if (phase < 1764) v += 0.5f * (float)sinf(2*M_PI*6000*t);
        for (int c = 0; c < ch; c++) b[(size_t)i*ch + c] = v;
    }
}

int main(void) {
    printf("=== Voice polish test (R015 Tier 1) ===\n\n");
    int sr = 44100, ch = 2, frames = 44100 * 2; /* 2 s */
    float *buf = (float*)malloc((size_t)frames * ch * sizeof(float));
    make_signal(buf, frames, ch);

    float before = wb_loudness_measure(buf, frames, ch, sr);
    CHECK(before > -80.0f && before < 0.0f, "loudness measure sane before");

    /* capture peak before */
    float peak0 = 0;
    for (uint32_t i = 0; i < (uint32_t)frames*ch; i++)
        if (fabsf(buf[i]) > peak0) peak0 = fabsf(buf[i]);

    int rc = wb_voice_polish_apply(buf, frames, ch, sr, -16.0f);
    CHECK(rc == 0, "apply returned 0");

    float after = wb_loudness_measure(buf, frames, ch, sr);
    printf("         loudness before=%.1f LUFS  after=%.1f LUFS (target -16)\n",
           before, after);
    CHECK(after > -22.0f && after < -10.0f, "loudness pulled toward -16 LUFS");

    /* no clipping / NaN */
    int nanc = 0; float peak1 = 0;
    for (uint32_t i = 0; i < (uint32_t)frames*ch; i++) {
        if (isnan(buf[i]) || isinf(buf[i])) nanc++;
        if (fabsf(buf[i]) > peak1) peak1 = fabsf(buf[i]);
    }
    CHECK(nanc == 0, "no NaN/Inf after processing");
    CHECK(peak1 <= 1.0001f, "no clipping (peak <= 1.0)");
    CHECK(peak1 > 0.0f, "signal not killed to silence");
    CHECK(peak1 <= peak0 + 0.001f, "limiter did not amplify past input peak");

    /* sibilance band should be reduced vs non-sibilant energy (de-esser works).
     * Compare RMS of high-band burst windows before/after. */
    float rms_hi_before = 0, rms_hi_after = 0; int cnt = 0;
    wb_biquad bp0, bp1;
    wb_biquad_init(&bp0, sr); wb_biquad_set(&bp0, 2, 6000, 1.2f, 0);
    wb_biquad_init(&bp1, sr); wb_biquad_set(&bp1, 2, 6000, 1.2f, 0);
    /* recompute a fresh signal copy for the "before" high-band reference */
    float *ref = (float*)malloc((size_t)frames*ch*sizeof(float));
    make_signal(ref, frames, ch);
    for (uint32_t i = 0; i < (uint32_t)frames; i++) {
        int phase = i % 22050;
        if (phase < 1764) {
            float b0 = wb_biquad_process(&bp0, ref[(size_t)i*ch]);
            float b1 = wb_biquad_process(&bp1, buf[(size_t)i*ch]);
            rms_hi_before += b0*b0; rms_hi_after += b1*b1; cnt++;
        }
    }
    rms_hi_before = sqrtf(rms_hi_before / cnt);
    rms_hi_after  = sqrtf(rms_hi_after  / cnt);
    printf("         sibilance band RMS  before=%.4f  after=%.4f\n",
           rms_hi_before, rms_hi_after);
    CHECK(rms_hi_after <= rms_hi_before + 1e-4f,
          "de-esser reduced sibilance band energy");

    free(ref);
    free(buf);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
