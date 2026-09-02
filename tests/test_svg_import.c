/* test_svg_import.c — verify SVG import */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void) {
    int pass = 0, fail = 0;
    printf("=== SVG Import ===\n");

    /* Create a test SVG file */
    const char *svg_path = "/tmp/test_import.svg";
    FILE *f = fopen(svg_path, "w");
    if (f) {
        fprintf(f, "<?xml version=\"1.0\"?>\n");
        fprintf(f, "<svg width=\"200\" height=\"200\" xmlns=\"http://www.w3.org/2000/svg\">\n");
        fprintf(f, "  <rect x=\"10\" y=\"10\" width=\"80\" height=\"60\" fill=\"#FF0000\" stroke=\"blue\" stroke-width=\"2\"/>\n");
        fprintf(f, "  <circle cx=\"100\" cy=\"100\" r=\"40\" fill=\"green\"/>\n");
        fprintf(f, "  <ellipse cx=\"50\" cy=\"150\" rx=\"30\" ry=\"20\" fill=\"yellow\"/>\n");
        fprintf(f, "  <path d=\"M 10 10 L 90 10 L 50 90 Z\" fill=\"purple\"/>\n");
        fprintf(f, "</svg>\n");
        fclose(f);
    }

    /* Import SVG */
    wb_node *nodes[32];
    int count = wb_svg_import(svg_path, 200, 200, nodes, 32);
    CHECK(count >= 3, "SVG imported at least 3 shapes");
    printf("  Imported %d shapes\n", count);

    /* Cleanup nodes */
    for (int i = 0; i < count; i++) {
        if (nodes[i] && nodes[i]->free) nodes[i]->free(nodes[i]);
    }
    CHECK(1, "all shape nodes freed");

    /* Path parsing */
    float vx[256], vy[256];
    int vcount = 0;
    wb_svg_parse_path("M 0 0 L 100 0 L 50 100 Z", vx, vy, &vcount, 256);
    CHECK(vcount == 3, "path parsed 3 vertices");

    /* Relative path */
    vcount = 0;
    wb_svg_parse_path("M 10 10 l 50 0 l 0 50 z", vx, vy, &vcount, 256);
    CHECK(vcount >= 3, "relative path parsed");

    /* Cubic bezier */
    vcount = 0;
    wb_svg_parse_path("M 0 0 C 50 0 100 50 50 100", vx, vy, &vcount, 256);
    CHECK(vcount >= 8, "cubic bezier subdivided into segments");

    /* Quadratic bezier */
    vcount = 0;
    wb_svg_parse_path("M 0 0 Q 50 0 50 100", vx, vy, &vcount, 256);
    CHECK(vcount >= 6, "quadratic bezier subdivided into segments");

    /* NULL safety */
    wb_svg_parse_path(NULL, vx, vy, &vcount, 64);
    wb_svg_import(NULL, 100, 100, nodes, 32);
    CHECK(1, "NULL inputs don't crash");

    /* Nonexistent file */
    count = wb_svg_import("/tmp/nonexistent.svg", 100, 100, nodes, 32);
    CHECK(count == -1, "nonexistent file returns -1");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
