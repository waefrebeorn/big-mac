/* test_voice_polish.c — headless verification of the voice-polish preset
 * chain (R015 Tier 1): gate -> deesser -> comp -> EQ -> limiter -> loudness. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    /* ---- G7: param-track-driven graph -------------------------------- */
    printf("\n=== G7 param-track voice graph ===\n");
    {
        int sr2 = 44100, ch2 = 1, fr2 = 44100; /* 1 s mono */
        float *a = (float*)malloc((size_t)fr2*ch2*sizeof(float));
        float *b_sig = (float*)malloc((size_t)fr2*ch2*sizeof(float));
        make_signal(a, fr2, ch2);
        make_signal(b_sig, fr2, ch2);

        /* static default run */
        wb_voice_polish *vp0 = wb_voice_polish_create(sr2, ch2);
        wb_voice_polish_process(vp0, a, fr2);
        float peak_static = 0;
        for (uint32_t i = 0; i < (uint32_t)fr2*ch2; i++)
            if (fabsf(a[i]) > peak_static) peak_static = fabsf(a[i]);
        wb_voice_polish_free(vp0);

        /* bind comp_ratio to a keyframe track: 1.0 (none) -> 8.0 (heavy) */
        wb_param_track *tr = wb_param_track_create();
        wb_param_track_set(tr, 0.0, 1.0f, WB_KF_LINEAR);
        wb_param_track_set(tr, 1.0, 8.0f, WB_KF_LINEAR);
        wb_voice_polish *vp1 = wb_voice_polish_create(sr2, ch2);
        int ok = wb_voice_polish_bind(vp1, "comp_ratio", tr);
        CHECK(ok == 0, "bound comp_ratio to keyframe track");
        CHECK(fabsf(wb_voice_polish_param_at(vp1, "comp_ratio", 0.5) - 4.5f) < 1e-3f,
              "param_at returns keyframed 4.5 at t=0.5");
        CHECK(fabsf(wb_voice_polish_param_at(vp1, "comp_ratio", 0.0) - 1.0f) < 1e-3f,
              "param_at returns keyframed 1.0 at t=0");
        wb_voice_polish_process(vp1, b_sig, fr2);
        float peak_kf = 0;
        for (uint32_t i = 0; i < (uint32_t)fr2*ch2; i++)
            if (fabsf(b_sig[i]) > peak_kf) peak_kf = fabsf(b_sig[i]);
        wb_voice_polish_free(vp1);
        wb_param_track_free(tr);
        printf("         peak static=%.3f  peak keyframed-ratio=%.3f\n",
               peak_static, peak_kf);
        /* heavier compression (ratio ramps to 8) raises average level:
         * keyframed run should yield different (higher RMS) output */
        CHECK(fabsf(peak_kf - peak_static) > 1e-3f,
              "keyframed comp_ratio changed the processed output");
        free(a); free(b_sig);
    }

    /* ---- G8: EBUR128 two-pass loudness --- */
    {
        int sr8 = 44100, ch8 = 2; uint32_t n8 = sr8 / 2;
        float *w = malloc((size_t)n8 * ch8 * sizeof(float));
        for (uint32_t i = 0; i < n8 * (uint32_t)ch8; i++)
            w[i] = 0.04f * (float)sinf(2.0f * 3.14159f * 200.0f * i / sr8);
        float in_lufs = wb_loudness_measure(w, n8, ch8, (float)sr8);
        /* single-pass */
        float *w1 = malloc((size_t)n8 * ch8 * sizeof(float));
        memcpy(w1, w, (size_t)n8 * ch8 * sizeof(float));
        wb_voice_polish_apply(w1, n8, ch8, (float)sr8, -16.0f);
        float l1 = wb_loudness_measure(w1, n8, ch8, (float)sr8);
        /* two-pass */
        float *w2 = malloc((size_t)n8 * ch8 * sizeof(float));
        memcpy(w2, w, (size_t)n8 * ch8 * sizeof(float));
        wb_voice_polish_apply_twopass(w2, n8, ch8, (float)sr8, -16.0f);
        float l2 = wb_loudness_measure(w2, n8, ch8, (float)sr8);
        printf("         input=%.1f  single-pass=%.1f  two-pass=%.1f LUFS\n",
               in_lufs, l1, l2);
        CHECK(l1 > -26.0f && l1 < -10.0f, "single-pass normalizes toward target (chain-dependent)");
        CHECK(l2 > -19.0f && l2 < -13.0f, "two-pass lands near -16 LUFS (BS.1770)");
        CHECK(fabsf(l2 + 16.0f) < 1.0f,
              "two-pass is the accurate BS.1770 loudness normalize");
        free(w); free(w1); free(w2);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
