/* wb_mesh.c — primitive mesh library + .obj loader (R053).
 *
 * Every primitive is built from the same growable vertex/tri lists; the
 * wb_rast scene format is the native output. Flat colors, Lambert-ish
 * brightness variation on curved primitives so shapes read as 3D even
 * without per-pixel lighting (Jet's "bake lighting into static objects").
 */

#include "wbus/wbus_mesh.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef WB_MESH_PI
#define WB_MESH_PI 3.14159265358979
#endif

struct wb_mesh {
    wb_rast_vertex *verts; int nverts, capv;
    wb_rast_tri    *tris;  int ntris,  capt;
};

wb_mesh *wb_mesh_create(void) {
    return calloc(1, sizeof(wb_mesh));
}

void wb_mesh_free(wb_mesh *m) {
    if (!m) return;
    free(m->verts); free(m->tris);
    free(m);
}

static int mesh_add_vert(wb_mesh *m, float x, float y, float z) {
    if (m->nverts >= m->capv) {
        int nc = m->capv ? m->capv * 2 : 64;
        wb_rast_vertex *nv = realloc(m->verts, (size_t)nc * sizeof(*nv));
        if (!nv) return -1;
        m->verts = nv; m->capv = nc;
    }
    m->verts[m->nverts].x = x; m->verts[m->nverts].y = y; m->verts[m->nverts].z = z;
    return m->nverts++;
}

static void mesh_add_tri(wb_mesh *m, int a, int b, int c,
                         uint8_t r, uint8_t g, uint8_t bl) {
    if (m->ntris >= m->capt) {
        int nc = m->capt ? m->capt * 2 : 128;
        wb_rast_tri *nt = realloc(m->tris, (size_t)nc * sizeof(*nt));
        if (!nt) return;
        m->tris = nt; m->capt = nc;
    }
    m->tris[m->ntris].v0 = a; m->tris[m->ntris].v1 = b; m->tris[m->ntris].v2 = c;
    m->tris[m->ntris].r = r; m->tris[m->ntris].g = g; m->tris[m->ntris].b = bl;
    m->ntris++;
}

wb_mesh *wb_mesh_build(const wb_rast_vertex *verts, int nverts,
                       const wb_rast_tri *tris, int ntris) {
    if (!verts || !tris || nverts <= 0 || ntris <= 0) return NULL;
    wb_mesh *m = wb_mesh_create();
    if (!m) return NULL;
    for (int i = 0; i < nverts; i++)
        if (mesh_add_vert(m, verts[i].x, verts[i].y, verts[i].z) < 0) {
            wb_mesh_free(m); return NULL;
        }
    for (int i = 0; i < ntris; i++) {
        if (tris[i].v0 >= m->nverts || tris[i].v1 >= m->nverts ||
            tris[i].v2 >= m->nverts) { wb_mesh_free(m); return NULL; }
        mesh_add_tri(m, tris[i].v0, tris[i].v1, tris[i].v2,
                     tris[i].r, tris[i].g, tris[i].b);
    }
    return m;
}

wb_mesh *wb_mesh_copy(const wb_mesh *m) {
    if (!m) return NULL;
    return wb_mesh_build(m->verts, m->nverts, m->tris, m->ntris);
}

const wb_rast_vertex *wb_mesh_vert_src(const wb_mesh *m) { return m ? m->verts : NULL; }
const wb_rast_tri    *wb_mesh_tri_src(const wb_mesh *m)  { return m ? m->tris : NULL; }

int wb_mesh_vert_count(const wb_mesh *m) { return m ? m->nverts : 0; }
int wb_mesh_tri_count(const wb_mesh *m)  { return m ? m->ntris : 0; }

void wb_mesh_emit(const wb_mesh *m, wb_rast_vertex *verts, wb_rast_tri *tris) {
    if (!m) return;
    if (verts) memcpy(verts, m->verts, (size_t)m->nverts * sizeof(*verts));
    if (tris)  memcpy(tris,  m->tris,  (size_t)m->ntris  * sizeof(*tris));
}

/* shade(base, f): scale a base color by brightness factor f (clamped). */
static uint8_t shade(uint8_t base, float f) {
    int v = (int)(base * f);
    return (uint8_t)(v > 255 ? 255 : v);
}

/* ---- primitives --------------------------------------------------------- */

wb_mesh *wb_mesh_box(float hx, float hy, float hz,
                     uint8_t r, uint8_t g, uint8_t b) {
    wb_mesh *m = wb_mesh_create();
    if (!m) return NULL;
    static const float cv[8][3] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}
    };
    for (int i = 0; i < 8; i++)
        mesh_add_vert(m, cv[i][0]*hx, cv[i][1]*hy, cv[i][2]*hz);
    /* face quads with distinct brightness per axis so the box reads as 3D */
    static const int faces[6][4] = {
        {0,3,2,1},{4,5,6,7},{0,1,5,4},{2,3,7,6},{0,4,7,3},{1,2,6,5}
    };
    static const float fb[6] = { 0.55f, 1.00f, 0.75f, 0.75f, 0.60f, 0.90f };
    for (int f = 0; f < 6; f++) {
        uint8_t rr = shade(r, fb[f]), gg = shade(g, fb[f]), bb = shade(b, fb[f]);
        mesh_add_tri(m, faces[f][0], faces[f][1], faces[f][2], rr, gg, bb);
        mesh_add_tri(m, faces[f][0], faces[f][2], faces[f][3], rr, gg, bb);
    }
    return m;
}

wb_mesh *wb_mesh_sphere(float radius, int lat_bands, int lon_bands,
                        uint8_t r, uint8_t g, uint8_t b) {
    if (lat_bands < 2) lat_bands = 2;
    if (lon_bands < 3) lon_bands = 3;
    wb_mesh *m = wb_mesh_create();
    if (!m) return NULL;
    /* vertex grid: (lat+1) rings x lon columns */
    for (int la = 0; la <= lat_bands; la++) {
        float theta = (float)la / lat_bands * WB_MESH_PI;      /* 0..pi   */
        float st = sinf(theta), ct = cosf(theta);
        for (int lo = 0; lo < lon_bands; lo++) {
            float phi = (float)lo / lon_bands * 2.0f * WB_MESH_PI;
            mesh_add_vert(m, radius*st*cosf(phi), radius*ct, radius*st*sinf(phi));
        }
    }
    float top_b = 1.15f, bot_b = 0.55f;
    for (int la = 0; la < lat_bands; la++) {
        float f = top_b - (top_b - bot_b) * (float)la / lat_bands;
        uint8_t rr = shade(r, f), gg = shade(g, f), bb = shade(b, f);
        for (int lo = 0; lo < lon_bands; lo++) {
            int a = la*lon_bands + lo;
            int bq = la*lon_bands + (lo+1)%lon_bands;
            int c = (la+1)*lon_bands + lo;
            int d = (la+1)*lon_bands + (lo+1)%lon_bands;
            if (la != 0)               mesh_add_tri(m, a, d, c, rr, gg, bb);
            if (la != lat_bands-1)     mesh_add_tri(m, a, bq, d, rr, gg, bb);
        }
    }
    return m;
}

wb_mesh *wb_mesh_cylinder(float radius, float height, int segs,
                          uint8_t r, uint8_t g, uint8_t b) {
    if (segs < 3) segs = 3;
    wb_mesh *m = wb_mesh_create();
    if (!m) return NULL;
    float hh = height * 0.5f;
    for (int i = 0; i < segs; i++) {
        float a = (float)i/segs * 2.0f * WB_MESH_PI;
        float ca = cosf(a)*radius, sa = sinf(a)*radius;
        mesh_add_vert(m, ca, -hh, sa);          /* bottom ring */
        mesh_add_vert(m, ca,  hh, sa);          /* top ring    */
    }
    int bcap = mesh_add_vert(m, 0, -hh, 0);     /* bottom center */
    int tcap = mesh_add_vert(m, 0,  hh, 0);     /* top center    */
    for (int i = 0; i < segs; i++) {
        int j = (i+1)%segs;
        int b0 = i*2, t0 = i*2+1, b1 = j*2, t1 = j*2+1;
        float f = 0.65f + 0.35f * (0.5f + 0.5f*cosf((float)i/segs*2*WB_MESH_PI));
        uint8_t rr = shade(r,f), gg = shade(g,f), bb = shade(b,f);
        mesh_add_tri(m, b0, b1, t1, rr, gg, bb);
        mesh_add_tri(m, b0, t1, t0, rr, gg, bb);
        mesh_add_tri(m, bcap, b1, b0, shade(r,0.5f), shade(g,0.5f), shade(b,0.5f));
        mesh_add_tri(m, tcap, t0, t1, shade(r,1.0f), shade(g,1.0f), shade(b,1.0f));
    }
    return m;
}

wb_mesh *wb_mesh_cone(float radius, float height, int segs,
                      uint8_t r, uint8_t g, uint8_t b) {
    if (segs < 3) segs = 3;
    wb_mesh *m = wb_mesh_create();
    if (!m) return NULL;
    float hh = height * 0.5f;
    int apex = mesh_add_vert(m, 0, hh, 0);
    int bcap = mesh_add_vert(m, 0, -hh, 0);
    for (int i = 0; i < segs; i++) {
        float a = (float)i/segs * 2.0f * WB_MESH_PI;
        mesh_add_vert(m, cosf(a)*radius, -hh, sinf(a)*radius);
    }
    for (int i = 0; i < segs; i++) {
        int j = (i+1)%segs;
        int p0 = 2+i, p1 = 2+j;
        float f = 0.60f + 0.40f * (0.5f + 0.5f*sinf((float)i/segs*WB_MESH_PI));
        mesh_add_tri(m, apex, p1, p0, shade(r,f), shade(g,f), shade(b,f));
        mesh_add_tri(m, bcap, p0, p1, shade(r,0.45f), shade(g,0.45f), shade(b,0.45f));
    }
    return m;
}

wb_mesh *wb_mesh_torus(float R, float r, int maj, int min,
                       uint8_t r8, uint8_t g8, uint8_t b8) {
    if (maj < 3) maj = 3;
    if (min < 3) min = 3;
    wb_mesh *m = wb_mesh_create();
    if (!m) return NULL;
    for (int i = 0; i < maj; i++) {
        float u = (float)i/maj * 2.0f * WB_MESH_PI;
        float cu = cosf(u), su = sinf(u);
        for (int j = 0; j < min; j++) {
            float v = (float)j/min * 2.0f * WB_MESH_PI;
            float cv = cosf(v), sv = sinf(v);
            mesh_add_vert(m, (R + r*cv)*cu, r*sv, (R + r*cv)*su);
        }
    }
    for (int i = 0; i < maj; i++) {
        float f = 0.55f + 0.45f * (0.5f + 0.5f*sinf((float)i/maj*WB_MESH_PI));
        uint8_t rr = shade(r8,f), gg = shade(g8,f), bb = shade(b8,f);
        for (int j = 0; j < min; j++) {
            int a = i*min + j;
            int bq = i*min + (j+1)%min;
            int k = ((i+1)%maj)*min + j;
            int d = ((i+1)%maj)*min + (j+1)%min;
            mesh_add_tri(m, a, bq, d, rr, gg, bb);
            mesh_add_tri(m, a, d, k, rr, gg, bb);
        }
    }
    return m;
}

wb_mesh *wb_mesh_plane(float half, uint8_t r, uint8_t g, uint8_t b) {
    wb_mesh *m = wb_mesh_create();
    if (!m) return NULL;
    mesh_add_vert(m, -half, 0, -half);
    mesh_add_vert(m,  half, 0, -half);
    mesh_add_vert(m,  half, 0,  half);
    mesh_add_vert(m, -half, 0,  half);
    mesh_add_tri(m, 0, 1, 2, r, g, b);
    mesh_add_tri(m, 0, 2, 3, r, g, b);
    return m;
}

wb_mesh *wb_mesh_arrow(float shaft_r, float head_r, float len,
                       uint8_t r, uint8_t g, uint8_t b) {
    /* shaft = 70% of length, head = remaining 30% (min head 0.25) */
    float hl = len * 0.3f; if (hl < 0.25f) hl = 0.25f;
    float sl = len - hl;
    wb_mesh *shaft = wb_mesh_cylinder(shaft_r, sl, 10, r, g, b);
    wb_mesh *head  = wb_mesh_cone(head_r, hl, 10, r, g, b);
    if (!shaft || !head) { wb_mesh_free(shaft); wb_mesh_free(head); return NULL; }
    wb_mesh_translate(head, 0, sl/2 + hl/2, 0);
    wb_mesh *m = wb_mesh_create();
    if (!m) { wb_mesh_free(shaft); wb_mesh_free(head); return NULL; }
    wb_mesh_append(m, shaft);
    wb_mesh_append(m, head);
    wb_mesh_free(shaft); wb_mesh_free(head);
    return m;
}

/* ---- transforms ---------------------------------------------------------- */

void wb_mesh_translate(wb_mesh *m, float dx, float dy, float dz) {
    if (!m) return;
    for (int i = 0; i < m->nverts; i++) {
        m->verts[i].x += dx; m->verts[i].y += dy; m->verts[i].z += dz;
    }
}

void wb_mesh_scale(wb_mesh *m, float sx, float sy, float sz) {
    if (!m) return;
    for (int i = 0; i < m->nverts; i++) {
        m->verts[i].x *= sx; m->verts[i].y *= sy; m->verts[i].z *= sz;
    }
}

void wb_mesh_rotate_y(wb_mesh *m, float radians) {
    if (!m) return;
    float c = cosf(radians), s = sinf(radians);
    for (int i = 0; i < m->nverts; i++) {
        float x = m->verts[i].x, z = m->verts[i].z;
        m->verts[i].x = x*c + z*s;
        m->verts[i].z = -x*s + z*c;
    }
}

void wb_mesh_paint(wb_mesh *m, uint8_t r, uint8_t g, uint8_t b) {
    if (!m) return;
    for (int i = 0; i < m->ntris; i++) {
        m->tris[i].r = r; m->tris[i].g = g; m->tris[i].b = b;
    }
}

int wb_mesh_append(wb_mesh *dst, const wb_mesh *src) {
    if (!dst || !src) return -1;
    int base = dst->nverts;
    for (int i = 0; i < src->nverts; i++)
        if (mesh_add_vert(dst, src->verts[i].x, src->verts[i].y, src->verts[i].z) < 0)
            return -1;
    for (int i = 0; i < src->ntris; i++) {
        wb_rast_tri *t = &src->tris[i];
        mesh_add_tri(dst, base + t->v0, base + t->v1, base + t->v2,
                     t->r, t->g, t->b);
    }
    return 0;
}

/* ---- .obj loader (v + f lines only; polygons triangulated as fans) ------ */

wb_mesh *wb_mesh_load_obj(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    wb_mesh *m = wb_mesh_create();
    if (!m) { fclose(f); return NULL; }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && (line[1] == ' ' || line[1] == '\t')) {
            float x, y, z;
            if (sscanf(line + 2, "%f %f %f", &x, &y, &z) == 3)
                mesh_add_vert(m, x, y, z);
        } else if (line[0] == 'f' && (line[1] == ' ' || line[1] == '\t')) {
            /* face: indices are 1-based, may be v/vt/vn — take the first */
            int idx[64]; int ni = 0;
            char *p = line + 2;
            while (*p && ni < 64) {
                char *end;
                long v = strtol(p, &end, 10);
                if (end == p) break;
                idx[ni++] = (int)v - 1;
                /* skip to next whitespace run */
                while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
                while (*p == ' ' || *p == '\t') p++;
            }
            /* fan-triangulate; negative (relative) indices unsupported */
            for (int i = 1; i + 1 < ni; i++)
                mesh_add_tri(m, idx[0], idx[i], idx[i+1], 200, 200, 200);
        }
    }
    fclose(f);
    if (m->ntris == 0) { wb_mesh_free(m); return NULL; }
    return m;
}

/* ---- OBJ export (R056) -------------------------------------------------- */

int wb_mesh_write_obj(const wb_mesh *m, const char *obj_path) {
    if (!m || !obj_path || m->nverts == 0) return -1;
    char mtl_path[1024];
    snprintf(mtl_path, sizeof mtl_path, "%s", obj_path);
    char *dot = strrchr(mtl_path, '.');
    if (dot) *dot = 0;
    char mtl_file[1100];
    snprintf(mtl_file, sizeof mtl_file, "%s.mtl", mtl_path);

    FILE *mf = fopen(mtl_file, "w");
    FILE *f = fopen(obj_path, "w");
    if (!f || !mf) { if(f)fclose(f); if(mf)fclose(mf); return -1; }

    /* one material per distinct color (meshes have few) */
    fprintf(f, "mtllib %s.mtl\n", mtl_path);
    for (int i = 0; i < m->nverts; i++)
        fprintf(f, "v %f %f %f\n", m->verts[i].x, m->verts[i].y, m->verts[i].z);

    int written = 0;
    for (int i = 0; i < m->ntris; i++) {
        wb_rast_tri *t = &m->tris[i];
        char matname[32];
        snprintf(matname, sizeof matname, "mat_%02x%02x%02x", t->r, t->g, t->b);
        fprintf(f, "usemtl %s\n", matname);
        fprintf(f, "f %d %d %d\n",
                t->v0+1, t->v1+1, t->v2+1);
        written++;
        (void)written;
    }
    /* write materials deduped */
    for (int i = 0; i < m->ntris; i++) {
        wb_rast_tri *t = &m->tris[i];
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (m->tris[j].r==t->r && m->tris[j].g==t->g && m->tris[j].b==t->b)
            { seen = 1; break; }
        if (seen) continue;
        fprintf(mf, "newmtl mat_%02x%02x%02x\nKd %.4f %.4f %.4f\n",
                t->r, t->g, t->b,
                t->r/255.0f, t->g/255.0f, t->b/255.0f);
    }
    fclose(f); fclose(mf);
    return 0;
}

/* ---- R074 hop 118 (G-SF011): capsule + wedge primitives --------------- */
/* Capsule: cylinder of `height` (Y axis) capped by two hemispheres. */
wb_mesh *wb_mesh_capsule(float radius, float height, int segs,
                         uint8_t r, uint8_t g, uint8_t b) {
    wb_mesh *cyl = wb_mesh_cylinder(radius, height, segs, r,g,b);
    if (!cyl) return NULL;
    wb_mesh *top = wb_mesh_sphere(radius, segs/2 < 2 ? 2 : segs/2,
                                  segs, r,g,b);
    wb_mesh *bot = wb_mesh_sphere(radius, segs/2 < 2 ? 2 : segs/2,
                                  segs, r,g,b);
    if (!top || !bot) {
        wb_mesh_free(cyl);
        if (top) wb_mesh_free(top);
        if (bot) wb_mesh_free(bot);
        return NULL;
    }
    /* squash spheres to hemispheres and shift */
    wb_mesh_scale(top, 1.0f, 0.5f, 1.0f);
    wb_mesh_scale(bot, 1.0f, 0.5f, 1.0f);
    wb_mesh_translate(top, 0,  height/2, 0);
    wb_mesh_translate(bot, 0, -height/2, 0);
    /* merge: rebuild from combined arrays */
    int nv = wb_mesh_vert_count(cyl)+wb_mesh_vert_count(top)
            +wb_mesh_vert_count(bot);
    int nt = wb_mesh_tri_count(cyl)+wb_mesh_tri_count(top)
            +wb_mesh_tri_count(bot);
    wb_rast_vertex *v = malloc(sizeof(*v)*(size_t)nv);
    wb_rast_tri *t = malloc(sizeof(*t)*(size_t)nt);
    if (!v || !t) { free(v); free(t);
        wb_mesh_free(cyl); wb_mesh_free(top); wb_mesh_free(bot);
        return NULL; }
    int vo = 0, to = 0;
    const wb_mesh *parts[3] = { cyl, top, bot };
    for (int p = 0; p < 3; p++) {
        const wb_rast_vertex *pv = wb_mesh_vert_src(parts[p]);
        const wb_rast_tri *pt = wb_mesh_tri_src(parts[p]);
        int base = vo;
        for (int i = 0; i < wb_mesh_vert_count(parts[p]); i++)
            v[vo++] = pv[i];
        for (int i = 0; i < wb_mesh_tri_count(parts[p]); i++) {
            t[to] = pt[i];
            t[to].v0 += base; t[to].v1 += base; t[to].v2 += base;
            to++;
        }
    }
    wb_mesh *outm = wb_mesh_build(v, nv, t, nt);
    free(v); free(t);
    wb_mesh_free(cyl); wb_mesh_free(top); wb_mesh_free(bot);
    return outm;
}

/* Wedge: right-triangle prism along Z (ramp). Base in XY plane. */
wb_mesh *wb_mesh_wedge(float hx, float hy, float hz,
                       uint8_t r, uint8_t g, uint8_t b) {
    /* 6 vertices: front triangle + back triangle + 3 quads (as tris) */
    wb_rast_vertex v[6] = {
        {-hx,-hy,-hz}, {hx,-hy,-hz}, {-hx,hy,-hz},   /* front tri */
        {-hx,-hy, hz}, {hx,-hy, hz}, {-hx,hy, hz},   /* back tri */
    };
    wb_rast_tri t[8] = {
        {0,1,2,r,g,b},      /* front */
        {5,4,3,r,g,b},      /* back */
        {0,2,5,r,g,b}, {0,5,3,r,g,b},   /* slope face */
        {0,3,4,r,g,b}, {0,4,1,r,g,b},   /* bottom */
        {1,4,5,r,g,b}, {1,5,2,r,g,b},   /* vertical back face */
    };
    return wb_mesh_build(v, 6, t, 8);
}
