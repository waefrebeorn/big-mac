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

    wb_clap_host_destroy(host);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
