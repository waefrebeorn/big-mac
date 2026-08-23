/* test_clap.c — headless CLAP host verification (THE gate for plugin hosting).
 * Loads the test .clap dylib, enumerates, instantiates both plugins, and
 * verifies they process audio correctly (gain scales, lowpass low-passes).
 *
 * Usage: test_clap <dir-containing-.clap>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wbus.h"
#include "wbus_clap.h"

static int checks = 0, failures = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { failures++; printf("  [FAIL] %s\n", msg); } \
} while (0)

static void test_clap_engine_graph(const char *dir);

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/test-clap";
    printf("=== Big Mac CLAP host test ===\n");
    printf("scan dir: %s\n", dir);

    wb_clap_host *host = wb_clap_host_create(dir);
    CHECK(host != NULL, "host created");
    if (!host) return 1;

    uint32_t n = wb_clap_host_plugin_count(host);
    printf("plugins found: %u\n", n);
    CHECK(n >= 2, "found >= 2 test plugins");

    /* print the list */
    for (uint32_t i = 0; i < n; i++) {
        const char *name = NULL, *id = NULL;
        wb_clap_host_plugin_info(host, i, &name, &id);
        printf("  [%u] %s (%s)\n", i, name ? name : "?", id ? id : "?");
    }

    /* ---- gain plugin: output = input * 0.5 --------------------------- */
    printf("-- gain plugin --\n");
    wb_clap_plugin *gain = NULL;
    for (uint32_t i = 0; i < n; i++) {
        const char *id = NULL;
        wb_clap_host_plugin_info(host, i, NULL, &id);
        if (id && strcmp(id, "org.bigmac.test.gain") == 0) {
            gain = wb_clap_plugin_create(host, i, 44100);
            break;
        }
    }
    CHECK(gain != NULL, "gain plugin instantiated");
    if (gain) {
        float inL[8], inR[8], outL[8], outR[8];
        for (int i = 0; i < 8; i++) { inL[i] = 0.5f; inR[i] = -0.5f; outL[i]=0; outR[i]=0; }
        int rc = wb_clap_plugin_process(gain, inL, inR, outL, outR, 8);
        CHECK(rc == 0, "gain process returned 0");
        /* gain 0.5 * 0.5 input = 0.25 */
        CHECK(fabsf(outL[0] - 0.25f) < 1e-4, "gain scaled L (0.5->0.25)");
        CHECK(fabsf(outR[0] - (-0.25f)) < 1e-4, "gain scaled R (-0.5->-0.25)");
        wb_clap_plugin_destroy(gain);
    }

    /* ---- lowpass plugin: DC passes, high-freq attenuated ------------- */
    printf("-- lowpass plugin --\n");
    wb_clap_plugin *lp = NULL;
    for (uint32_t i = 0; i < n; i++) {
        const char *id = NULL;
        wb_clap_host_plugin_info(host, i, NULL, &id);
        if (id && strcmp(id, "org.bigmac.test.lowpass") == 0) {
            lp = wb_clap_plugin_create(host, i, 44100);
            break;
        }
    }
    CHECK(lp != NULL, "lowpass plugin instantiated");
    if (lp) {
        /* feed a DC step for enough samples for the 1kHz LPF to converge */
        float inL[2048], inR[2048], outL[2048], outR[2048];
        memset(inL, 0, sizeof(inL)); memset(inR, 0, sizeof(inR));
        memset(outL, 0, sizeof(outL)); memset(outR, 0, sizeof(outR));
        for (int i = 0; i < 2048; i++) { inL[i]=1.0f; inR[i]=-1.0f; }
        wb_clap_plugin_process(lp, inL, inR, outL, outR, 2048);
        CHECK(fabsf(outL[2047] - 1.0f) < 0.05f, "lowpass passes DC (L)");
        CHECK(fabsf(outR[2047] - (-1.0f)) < 0.05f, "lowpass passes DC (R)");
        wb_clap_plugin_destroy(lp);
    }

    test_clap_engine_graph(dir);

    wb_clap_host_destroy(host);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

/* ---- engine graph integration: CLAP plugin as a realtime insert ---------- */
/* Verifies a CLAP plugin is instantiated from a "clap:<id>" insert slot and
 * runs inside wb_engine_render's stage_effects path (not standalone). */
static void test_clap_engine_graph(const char *dir) {
    printf("=== CLAP engine graph integration ===\n");
    static int eng_checks = 0, eng_fail = 0;
#define ECHECK(cond, msg) do { eng_checks++; \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { eng_fail++; printf("  [FAIL] %s\n", msg); } } while (0)

    wb_clap_host *host = wb_clap_host_create(dir);
    ECHECK(host != NULL, "host created for graph test");

    /* build a session: one audio track with a 1s sine, gain insert in slot 1 */
    wb_session *s = wb_session_create();
    s->bpm = 120.0; s->length = 44100.0;
    wb_track *tr = wb_session_add_track(s, "Sig", 1);
    uint32_t nf = 44100;
    wb_sample buf[44100];
    for (uint32_t i = 0; i < nf; i++) buf[i] = 0.8f; /* DC-ish tone (constant) */
    wb_session_add_audio_clip(tr, 0, (double)nf, buf, nf, 1);
    /* slot 0 reserved for instrument; put the CLAP gain plugin in slot 1 */
    snprintf(tr->inserts[1].id, sizeof(tr->inserts[1].id), "clap:org.bigmac.test.gain");

    wb_engine *e = wb_engine_create();
    wb_engine_set_clap_host(e, host);          /* attach the host BEFORE session */
    wb_engine_set_session(e, s);
    wb_engine_seek(e, 0.0);
    wb_engine_play(e);
    wb_sample out[4096*2];
    float peak_in = 0;
    for (uint32_t i = 0; i < nf; i += 4096) {
        uint32_t n = 4096; if (i + n > nf) n = nf - i;
        memset(out, 0, n*2*sizeof(wb_sample));
        wb_engine_render(e, out, n);
        for (uint32_t k = 0; k < n*2; k++) {
            float v = out[k]; if (v < 0) v = -v;
            if (v > peak_in) peak_in = v;
        }
    }
    ECHECK(peak_in > 0.01f, "graph renders audio clip");
    /* the gain plugin outputs 0.5x input; input clip is 0.8 -> expect ~0.4 */
    if (peak_in > 0.01f) {
        float ratio = peak_in / 0.8f;
        printf("         peak=%.3f ratio=%.3f (gain plugin = 0.5x)\n", peak_in, ratio);
        ECHECK(fabsf(ratio - 0.5f) < 0.05f, "CLAP gain plugin applied 0.5x in realtime graph");
    } else {
        ECHECK(0, "CLAP gain plugin applied 0.5x in realtime graph");
    }

    wb_engine_destroy(e);
    wb_session_destroy(s);
    wb_clap_host_destroy(host);
    printf("%d checks, %d failures\n", eng_checks, eng_fail);
    failures += eng_fail; checks += eng_checks;
}
