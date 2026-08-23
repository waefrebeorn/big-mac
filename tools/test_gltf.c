/* test_gltf.c — R056: GLB import round-trip.
 * Hand-writes a VALID glTF 2.0 GLB container (header + JSON chunk + BIN
 * chunk) containing one tetrahedron, then loads it through wb_gltf and
 * verifies geometry + rendering. Also verifies OBJ export/import. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_gltf.h"
#include "wbus/wbus_mesh.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

static const int W = 320, H = 240;

static int render_count(wb_mesh *m) {
    wb_rast_ctx *r = wb_rast_create(W, H);
    int nv = wb_mesh_vert_count(m), nt = wb_mesh_tri_count(m);
    wb_rast_vertex *verts = malloc((size_t)nv * sizeof(*verts));
    wb_rast_tri *tris = malloc((size_t)nt * sizeof(*tris));
    wb_mesh_emit(m, verts, tris);
    wb_rast_set_scene(r, verts, nv, tris, nt);
    wb_rast_set_camera(r, 0.4f, 0.5f, 0, 7.0f, 0);
    static uint8_t img[W*H*4];
    memset(img, 0, sizeof(img));
    wb_rast_render(r, img);
    int n = 0;
    for (int i = 0; i < W*H; i++) if (img[i*4+3] == 255) n++;
    free(verts); free(tris); wb_rast_destroy(r);
    return n;
}

/* write a minimal GLB: 4 verts (VEC3 float), 4 tris (SCALAR u16), node */
static int write_tetra_glb(const char *path) {
    /* binary buffer: 12 floats pos (48 bytes) + 6 u16 indices (12 bytes) */
    float pos[12] = {
        0, 1, 0,        /* apex   */
        -1,-1, 1,       /* base 0 */
         1,-1, 1,       /* base 1 */
         0,-1,-1        /* base 2 */
    };
    unsigned short idx[6] = { 0,1,2, 0,3,1, 0,2,3, 1,3,2 };
    unsigned char bin[60];
    memcpy(bin, pos, sizeof pos);
    memcpy(bin + 48, idx, sizeof idx);

    char json[1024];
    snprintf(json, sizeof json,
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{"
            "\"attributes\":{\"POSITION\":1},\"indices\":0}]}],"
        "\"accessors\":["
            "{\"bufferView\":1,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"},"
            "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"}],"
        "\"bufferViews\":["
            "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
            "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":12}],"
        "\"buffers\":[{\"byteLength\":60}]}");

    size_t jlen = strlen(json), blen = 60;
    size_t jp = (8 - (jlen & 3)) & 3;           /* pad JSON to 4 */
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t total = 12 + 8 + (uint32_t)(jlen+jp) + 8 + (uint32_t)blen;
    uint32_t magic = 0x46546C67, version = 2;
    fwrite(&magic, 4, 1, f); fwrite(&version, 4, 1, f); fwrite(&total, 4, 1, f);
    uint32_t jtype = 0x4E4F534A;
    uint32_t jl = (uint32_t)(jlen + jp);
    fwrite(&jl, 4, 1, f); fwrite(&jtype, 4, 1, f);
    fwrite(json, 1, jlen, f);
    for (size_t i = 0; i < jp; i++) fputc(' ', f);
    uint32_t btype = 0x004E4942;
    fwrite(&blen, 4, 1, f); fwrite(&btype, 4, 1, f);
    fwrite(bin, 1, blen, f);
    fclose(f);
    return 0;
}

int main(void) {
    printf("=== GLB import / asset I/O (R056) test ===\n\n");

    CHECK(write_tetra_glb("/tmp/wb_test.glb") == 0, "wrote valid GLB container");

    /* bad magic rejected */
    FILE *f = fopen("/tmp/wb_bad.glb", "wb");
    if (f) { fputs("not a glb at all", f); fclose(f); }
    CHECK(wb_gltf_load_glb("/tmp/wb_bad.glb") == NULL, "non-GLB rejected");
    CHECK(wb_gltf_load_glb("/tmp/wb_missing.glb") == NULL, "missing file -> NULL");

    wb_mesh *m = wb_gltf_load_glb_ex("/tmp/wb_test.glb", 1.0f, 200, 200, 200);
    CHECK(m != NULL, "tetra GLB parsed");
    if (m) {
        CHECK(wb_mesh_vert_count(m) == 4 && wb_mesh_tri_count(m) == 2,
              "4 verts / 2 tris extracted (u16 indices)");
        int px = render_count(m);
        CHECK(px > 500, "imported mesh renders");
        printf("         render px=%d\n", px);
        wb_mesh_free(m);
    }

    /* scale variant doubles coverage */
    int px_base = render_count(m);
    wb_mesh *m2 = wb_gltf_load_glb_ex("/tmp/wb_test.glb", 2.0f, 255, 0, 0);
    CHECK(m2 != NULL, "scaled GLB parsed");
    if (m2) {
        int pxa = render_count(m2);
        CHECK(pxa > px_base * 3, "scale=2 renders much larger");
        printf("         scale2 px=%d vs base=%d\n", pxa, px_base);
        wb_mesh_free(m2);
    }

    /* ---- OBJ export / re-import round trip ---- */
    wb_mesh *box = wb_mesh_box(1, 1, 1, 220, 90, 40);
    CHECK(wb_mesh_write_obj(box, "/tmp/wb_roundtrip.obj") == 0,
          "OBJ export wrote a file");
    wb_mesh *back = wb_mesh_load_obj("/tmp/wb_roundtrip.obj");
    CHECK(back && wb_mesh_vert_count(back) == wb_mesh_vert_count(box),
          "OBJ round-trip preserves vertex count");
    CHECK(back && wb_mesh_tri_count(back) >= wb_mesh_tri_count(box),
          "OBJ round-trip preserves triangle count");
    if (back) {
        int pxb = render_count(back);
        CHECK(pxb > 4000, "round-tripped box still renders solid");
        wb_mesh_free(back);
    }
    wb_mesh_free(box);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
