/* wb_scenedesc.c — R074 hop 171 (G-SF079): text scene description
 * loader. Pure C11, stdio only. */
#include "wbus/wbus_scenedesc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wb_mesh *make_prim(const char *kind, float a, float b, float c) {
    if (!strcmp(kind, "box"))    return wb_mesh_box(a, b, c, 200,200,200);
    if (!strcmp(kind, "cone"))   return wb_mesh_cone(a, b, (int)c, 220,220,220);
    if (!strcmp(kind, "sphere")) return wb_mesh_sphere(a, (int)b, (int)b/2, 210,210,210);
    return NULL;
}

int wb_scenedesc_load(wb_anim *a, const char *path) {
    if (!a || !path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int lineno = 0;
    int nobjs = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        char kind[32];
        float v[8] = {0};
        int nv = sscanf(p, "%31s %f %f %f %f %f %f %f %f",
                        kind, &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7]);
        if (nv <= 0) continue;
        if (!strcmp(kind, "box") || !strcmp(kind, "cone") ||
            !strcmp(kind, "sphere")) {
            wb_mesh *m = make_prim(kind, v[0], v[1], v[2]);
            if (!m) { fclose(f); return -lineno; }
            int id = wb_anim_add_object(a, m,
                                        (uint8_t)v[3],(uint8_t)v[4],
                                        (uint8_t)v[5]);
            if (id < 0) { fclose(f); return -lineno; }
            nobjs++;
            /* optional static key on the same line: x y z scale */
            if (nv >= 9)
                wb_anim_key(a, id, 0.0, v[6],v[7],0, 0,0,0, v[8]>0?v[8]:1);
            continue;
        }
        if (!strcmp(kind, "key")) {
            int obj = (int)v[0];
            double t = v[1];
            float sc = v[6] > 0 ? v[6] : 1.0f;
            if (wb_anim_key_ease(a, obj, t, v[2],v[3],v[4],
                                 0,0,0, sc, 1) < 0) {
                fclose(f); return -lineno;
            }
            continue;
        }
        if (!strcmp(kind, "shake")) {
            wb_anim_set_shake(a, v[0]);
            continue;
        }
        fclose(f); return -lineno;   /* unknown directive */
    }
    fclose(f);
    return nobjs > 0 ? 0 : -lineno;
}
