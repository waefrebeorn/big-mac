/* test_assets.c — R057: asset library index/cache verification.
 * Builds a synthetic kit tree (reusing the GLB writer approach from
 * test_gltf), scans it, lists kits/models, loads with cache, verifies. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "wbus/wbus_assets.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

static int write_tetra_glb(const char *path) {
    float pos[12] = { 0,1,0, -1,-1,1, 1,-1,1, 0,-1,-1 };
    unsigned short idx[6] = { 0,1,2, 0,3,1, 0,2,3, 1,3,2 };
    unsigned char bin[60];
    memcpy(bin, pos, 48);
    memcpy(bin + 48, idx, 12);

    char json[1024];
    snprintf(json, sizeof json,
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":1},\"indices\":0}]}],"
        "\"accessors\":["
            "{\"bufferView\":0,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"},"
            "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"}],"
        "\"bufferViews\":["
            "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":12},"
            "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48}],"
        "\"buffers\":[{\"byteLength\":60}]}");
    size_t jlen = strlen(json);
    size_t jp = (8 - (jlen & 3)) & 3;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t total = 12 + 8 + (uint32_t)(jlen+jp) + 8 + 60;
    uint32_t magic = 0x46546C67, version = 2;
    fwrite(&magic,4,1,f); fwrite(&version,4,1,f); fwrite(&total,4,1,f);
    uint32_t jtype = 0x4E4F534A, jl = (uint32_t)(jlen+jp);
    fwrite(&jl,4,1,f); fwrite(&jtype,4,1,f);
    fwrite(json,1,jlen,f);
    for (size_t i=0;i<jp;i++) fputc(' ',f);
    uint32_t btype = 0x004E4942, bl = 60;
    fwrite(&bl,4,1,f); fwrite(&btype,4,1,f);
    fwrite(bin,1,bl,f);
    fclose(f);
    return 0;
}

int main(void) {
    printf("=== Asset library (R057) test ===\n\n");

    /* build a fake library: kits/car-kit/{sedan,van}.glb + kits/nature/tree.glb */
    system("rm -rf /tmp/wb_lib && mkdir -p /tmp/wb_lib/car-kit /tmp/wb_lib/nature");
    CHECK(write_tetra_glb("/tmp/wb_lib/car-kit/sedan.glb") == 0, "wrote sedan.glb");
    CHECK(write_tetra_glb("/tmp/wb_lib/car-kit/van.glb") == 0, "wrote van.glb");
    CHECK(write_tetra_glb("/tmp/wb_lib/nature/tree.glb") == 0, "wrote tree.glb");

    wb_assets *a = wb_assets_open("/tmp/wb_lib");
    CHECK(a != NULL, "library opened");
    CHECK(wb_assets_kit_count(a) == 2, "2 kits indexed");
    CHECK(wb_assets_total(a) == 3, "3 models indexed total");

    int carkit = -1;
    for (int k = 0; k < wb_assets_kit_count(a); k++)
        if (strcmp(wb_assets_kit_name(a,k), "car-kit") == 0) carkit = k;
    CHECK(carkit >= 0, "car-kit found by name scan");
    CHECK(wb_assets_model_count(a, carkit) == 2, "car-kit has 2 models");

    wb_mesh *m1 = wb_assets_load(a, "car-kit", "sedan");
    CHECK(m1 != NULL, "sedan.glb loads through the library");
    CHECK(m1 && wb_mesh_tri_count(m1) == 2, "sedan geometry sane (2 tris)");

    /* cache: same pointer on second load */
    wb_mesh *m1b = wb_assets_load(a, "car-kit", "sedan");
    CHECK(m1b == m1, "cache returns the SAME mesh pointer");

    wb_mesh *miss = wb_assets_load(a, "car-kit", "nonexistent");
    CHECK(miss == NULL, "missing model -> NULL");

    wb_mesh *bad = wb_assets_load(a, "no-such-kit", "thing");
    CHECK(bad == NULL, "missing kit -> NULL");

    wb_assets_close(a);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
