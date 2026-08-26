/* wb_csg.c — R074 hop 188 (G-SF012): box subtraction via plane
 * clipping. Each triangle is clipped against the six half-spaces
 * (Sutherland-Hodgman); surviving fragments are appended to out.
 * Pure C11, deterministic. */
#include "wbus/wbus_csg.h"
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z; } vec3;

/* clip polygon against plane axis<op>value keeping the outside side.
 * inside[i] = 1 when vertex i is on the KEEP side. */
static int clip_poly(vec3 *p, int n, float *u, float *v, float *w,
                     int axis, float value, int keep_greater) {
    vec3 out[64]; float ou[64], ov[64], ow[64];
    int m = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float ci = (axis==0?p[i].x:axis==1?p[i].y:p[i].z) - value;
        float cj = (axis==0?p[j].x:axis==1?p[j].y:p[j].z) - value;
        if (!keep_greater) { ci = -ci; cj = -cj; }
        int ini = ci >= 0, inj = cj >= 0;
        if (ini) {
            out[m]=p[i]; ou[m]=u[i]; ov[m]=v[i]; ow[m]=w[i]; m++;
        }
        if (ini != inj) {
            float t = ci / (ci - cj);
            vec3 q = { p[i].x+(p[j].x-p[i].x)*t,
                       p[i].y+(p[j].y-p[i].y)*t,
                       p[i].z+(p[j].z-p[i].z)*t };
            out[m]=q;
            ou[m]=u[i]+(u[j]-u[i])*t;
            ov[m]=v[i]+(v[j]-v[i])*t;
            ow[m]=w[i]+(w[j]-w[i])*t;
            m++;
        }
        if (m > 60) return m;   /* safety cap */
    }
    memcpy(p, out, (size_t)m*sizeof(vec3));
    memcpy(u, ou, (size_t)m*sizeof(float));
    memcpy(v, ov, (size_t)m*sizeof(float));
    memcpy(w, ow, (size_t)m*sizeof(float));
    return m;
}

/* Emit one clipped fragment as a fan of triangles appended to out. */
static void emit_fragment(wb_mesh *out, const vec3 *poly,
                          const float *u, const float *v, const float *w,
                          int n, uint8_t sr, uint8_t sg, uint8_t sb,
                          uint8_t sa) {
    for (int i = 1; i + 1 < n; i++) {
        wb_rast_vertex fv[3] = {
            { poly[0].x,  poly[0].y,  poly[0].z  },
            { poly[i].x,  poly[i].y,  poly[i].z  },
            { poly[i+1].x,poly[i+1].y,poly[i+1].z}
        };
        float ww[3][3] = {
            { u[0],   v[0],   w[0]   },
            { u[i],   v[i],   w[i]   },
            { u[i+1], v[i+1], w[i+1] }
        };
        wb_rast_tri nt;
        nt.v0=0; nt.v1=1; nt.v2=2; nt.a=sa;
        uint8_t col[3];
        uint8_t corner[3] = { sr, sg, sb };
        for (int ci = 0; ci < 3; ci++) {
            float acc = ww[0][ci]*corner[ci]
                      + ww[1][ci]*corner[ci]
                      + ww[2][ci]*corner[ci];
            col[ci] = (uint8_t)(acc > 255 ? 255 : acc);
        }
        nt.r=col[0]; nt.g=col[1]; nt.b=col[2];
        wb_mesh *fm = wb_mesh_build(fv, 3, &nt, 1);
        if (fm) { wb_mesh_append(out, fm); wb_mesh_free(fm); }
    }
}

int wb_mesh_subtract_box(const wb_mesh *m,
                         float minx, float miny, float minz,
                         float maxx, float maxy, float maxz,
                         wb_mesh *out) {
    if (!m || !out) return -1;
    int nverts = wb_mesh_vert_count(m);
    (void)nverts;
    const wb_rast_vertex *V = wb_mesh_vert_src(m);
    for (int t = 0; t < wb_mesh_tri_count(m); t++) {
        const wb_rast_tri *tr = &wb_mesh_tri_src(m)[t];
        vec3 poly[64] = {
            { V[tr->v0].x, V[tr->v0].y, V[tr->v0].z },
            { V[tr->v1].x, V[tr->v1].y, V[tr->v1].z },
            { V[tr->v2].x, V[tr->v2].y, V[tr->v2].z }
        };
        /* barycentric-ish weights for color interpolation */
        float u[64] = {1,0,0}, v[64] = {0,1,0}, w[64] = {0,0,1};
        int n = 3;
        struct { int axis; float val; int keep_greater; } planes[6] = {
            {0,minx,1},{0,maxx,0},   /* keep x > minx ; keep x < maxx */
            {1,miny,1},{1,maxy,0},
            {2,minz,1},{2,maxz,0},
        };
        for (int pi = 0; pi < 6 && n >= 3; pi++)
            n = clip_poly(poly, n, u, v, w,
                          planes[pi].axis, planes[pi].val,
                          planes[pi].keep_greater);
        if (n < 3) continue;   /* fully inside the box: removed */
        emit_fragment(out, poly, u, v, w, n,
                      tr->r, tr->g, tr->b, tr->a);
    }
    return 0;
}
