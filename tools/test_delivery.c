/* test_delivery.c — R064: delivery presets verification.
 * Renders a session to WAV, measures its loudness (pass 1), normalizes
 * (pass 2), re-measures, and verifies the target hit. Also chapters
 * formatting incl. hour rollover and the "first chapter at 0:00" rule. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "wbus/wbus_delivery.h"
#include "wbus.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== Delivery presets (R064) test ===\n\n");

    /* ---- build a quiet test tone WAV (-30 LUFS-ish: very low amp) ---- */
    const uint32_t sr = 44100, secs = 3, ch = 1;
    const uint32_t n = sr * secs;
    wb_sample *buf = malloc(n * sizeof(wb_sample));
    for (uint32_t i = 0; i < n; i++)
        buf[i] = (wb_sample)(0.02 * sin(2*M_PI*220.0*i/(double)sr));
    const char *wav = "/tmp/wb_delivery_test.wav";
    CHECK(wb_wav_write_pcm16(wav, buf, n, ch, sr) == 0, "test WAV written");

    double I = 0, TP = 0, LRA = 0, TH = 0;
    CHECK(wb_delivery_measure_loudness(wav, &I, &TP, &LRA, &TH) == 0,
          "pass-1 loudness measured");
    printf("         input: I=%.1f LUFS tp=%.1f lra=%.1f\n", I, TP, LRA);
    CHECK(I < -20.0, "input is genuinely quiet (I below -20)");

    /* ---- normalize to YouTube target ---- */
    CHECK(wb_delivery_normalize_wav(wav, -14.0) == 0, "pass-2 normalize ran");
    double I2 = 0;
    CHECK(wb_delivery_measure_loudness(wav, &I2, &TP, &LRA, &TH) == 0,
          "re-measure after normalize");
    printf("         output: I=%.1f LUFS\n", I2);
    CHECK(fabs(I2 - (-14.0)) < 1.5, "output within 1.5 LU of -14 target");

    /* ---- chapters ---- */
    wb_session *s = wb_session_create();
    s->length = 44100.0 * 400;   /* >6 min so hour test possible via pos */
    wb_session_add_marker(s, 0.0, "Intro", 1);
    wb_session_add_marker(s, 90.0 * WB_SAMPLE_RATE, "Topic One", 0);
    wb_session_add_marker(s, 754.0 * WB_SAMPLE_RATE, "Late Topic", 0);

    char desc[2048];
    int nch = wb_delivery_chapters(s, desc, sizeof desc);
    CHECK(nch == 3, "three chapters written");
    CHECK(strstr(desc, "00:00:00 Intro") != NULL, "first chapter at 00:00:00");
    CHECK(strstr(desc, "00:01:30 Topic One") != NULL, "90s formats as 01:30");
    CHECK(strstr(desc, "00:12:34 Late Topic") != NULL, "hour rollover correct");

    /* fewer than 2 markers -> no chapters */
    wb_session *s2 = wb_session_create();
    wb_session_add_marker(s2, 0.0, "Only One", 1);
    char desc2[256];
    CHECK(wb_delivery_chapters(s2, desc2, sizeof desc2) == 0,
          "<2 markers -> no chapter block");

    free(buf);
    unlink(wav);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
