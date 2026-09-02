#include <stdio.h>
#include <stdlib.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

int main(void) {
    int pass = 0, fail = 0;
    printf("=== Compositor Export ===\n");

    wb_node *src = wb_node_source_color(0.2f, 0.5f, 0.9f, 1.0f, 320, 240);
    if (src) {
        printf("  PASS: source node created\n"); pass++;
    } else {
        printf("  FAIL: no source\n"); fail++; return 1;
    }

    printf("Exporting 1 second of 320x240 test pattern at 30fps...\n");
    int rc = wb_compositor_export_graph(src, 30.0, 1.0, "/tmp/test_export.mp4", 320, 240);
    if (rc == 0) {
        printf("  PASS: export succeeded\n"); pass++;
        /* Check file exists */
        FILE *f = fopen("/tmp/test_export.mp4", "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            printf("  PASS: output file exists (%ld bytes)\n", sz); pass++;
        } else {
            printf("  FAIL: output file not found\n"); fail++;
        }
    } else {
        printf("  FAIL: export returned %d\n", rc); fail++;
    }

    if (src->free) src->free(src);

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
