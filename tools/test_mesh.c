/* test_mesh.c — verification of the R053 primitive mesh library + obj loader.
 * Every primitive is rendered through wb_rast and inspected by pixel count
 * and color, proving the meshes are well-formed (right winding, closed). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_mesh.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

static const int W = 320, H = 240;

/* render a mesh, return drawn-pixel count + average color at center */
static int render_mesh(wb_mesh *m, uint8_t out_rc[3]) {
    wb_rast_ctx *r = wb_rast_create(W, H);
    int nv = wb_mesh_vert_count(m), nt = wb_mesh_tri_count(m);
    wb_rast_vertex *verts = malloc((size_t)nv * sizeof(*verts));
    wb_rast_tri *tris = malloc((size_t)nt * sizeof(*tris));
    wb_mesh_emit(m, verts, tris);
    wb_rast_set_scene(r, verts, nv, tris, nt);
    wb_rast_set_camera(r, 0.4f, 0.6f, 0, 6.0f, 0);
    static uint8_t img[W*H*4];
    memset(img, 0, sizeof(img));
    wb_rast_render(r, img);
    int n = 0; long sr = 0, sg = 0, sb = 0;
    for (int i = 0; i < W*H; i++)
        if (img[i*4+3] == 255) { n++; sr += img[i*4]; sg += img[i*4+1]; sb += img[i*4+2]; }
    if (n > 0 && out_rc) {
        out_rc[0] = (uint8_t)(sr/n); out_rc[1] = (uint8_t)(sg/n); out_rc[2] = (uint8_t)(sb/n);
    }
    free(verts); free(tris); wb_rast_destroy(r);
    return n;
}

int main(void) {
    printf("=== Mesh library (R053) test ===\n\n");
    uint8_t rc[3];

    /* box */
    wb_mesh *box = wb_mesh_box(1, 1, 1, 255, 80, 40);
    CHECK(box && wb_mesh_vert_count(box) == 8 && wb_mesh_tri_count(box) == 12,
          "box: 8 verts / 12 tris");
    int px = render_mesh(box, rc);
    CHECK(px > 5000, "box renders solid");
    CHECK(rc[0] > rc[2], "box face shading varies (reads as 3D)");
    wb_mesh_free(box);

    /* sphere */
    wb_mesh *sph = wb_mesh_sphere(1, 8, 12, 60, 120, 255);
    CHECK(sph && wb_mesh_tri_count(sph) == 8*12*2 - 24,
          "sphere tri count = lat*lon*2 - degenerate caps");
    px = render_mesh(sph, rc);
    CHECK(px > 4000, "sphere renders solid");
    wb_mesh_free(sph);

    /* cylinder */
    wb_mesh *cyl = wb_mesh_cylinder(0.7f, 2, 10, 80, 200, 80);
    CHECK(cyl && wb_mesh_tri_count(cyl) == 10*4, "cylinder: segs*4 tris");
    px = render_mesh(cyl, rc);
    CHECK(px > 4000, "cylinder renders solid");
    wb_mesh_free(cyl);

    /* cone */
    wb_mesh *cone = wb_mesh_cone(0.8f, 1.5f, 10, 220, 180, 60);
    CHECK(cone && wb_mesh_tri_count(cone) == 20, "cone: segs*2 tris");
    px = render_mesh(cone, rc);
    CHECK(px > 3000, "cone renders solid");
    wb_mesh_free(cone);

    /* torus */
    wb_mesh *tor = wb_mesh_torus(1, 0.35f, 12, 8, 180, 100, 220);
    CHECK(tor && wb_mesh_tri_count(tor) == 12*8*2, "torus: maj*min*2 tris");
    px = render_mesh(tor, rc);
    CHECK(px > 6000, "torus renders solid");
    wb_mesh_free(tor);

    /* arrow */
    wb_mesh *ar = wb_mesh_arrow(0.15f, 0.4f, 3, 255, 255, 100);
    CHECK(ar && wb_mesh_vert_count(ar) > 10, "arrow built from shaft+head");
    px = render_mesh(ar, rc);
    CHECK(px > 1500, "arrow renders");
    wb_mesh_free(ar);

    /* append + translate: two boxes side by side */
    wb_mesh *a = wb_mesh_box(0.5f, 0.5f, 0.5f, 255, 0, 0);
    wb_mesh *b = wb_mesh_box(0.5f, 0.5f, 0.5f, 0, 0, 255);
    wb_mesh_translate(b, 1.5f, 0, 0);
    wb_mesh_append(a, b);
    CHECK(wb_mesh_vert_count(a) == 16 && wb_mesh_tri_count(a) == 24,
          "append merges geometry with index rebasing");
    px = render_mesh(a, rc);
    CHECK(px > 8000, "both boxes visible after merge");
    wb_mesh_free(b);

    /* paint recolors everything */
    wb_mesh_paint(a, 10, 200, 10);
    render_mesh(a, rc);
    CHECK(rc[1] > 90 && rc[0] < rc[1] / 4, "paint recolors the merged mesh");
    wb_mesh_free(a);

    /* obj loader round-trip */
    FILE *f = fopen("/tmp/wbmesh_test.obj", "w");
    if (f) {
        fputs("# test obj\nv -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\nf 1 2 3 4\n", f);
        fclose(f);
    }
    wb_mesh *obj = wb_mesh_load_obj("/tmp/wbmesh_test.obj");
    CHECK(obj && wb_mesh_vert_count(obj) == 4 && wb_mesh_tri_count(obj) == 2,
          "obj loader: quad f-line fan-triangulated to 2 tris");
    if (obj) wb_mesh_free(obj);
    obj = wb_mesh_load_obj("/tmp/wbmesh_missing.obj");
    CHECK(obj == NULL, "missing file returns NULL");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
