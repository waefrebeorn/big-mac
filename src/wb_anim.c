/* wb_anim.c — keyframed 3D animation layer (R054).
 *
 * Per-object keyframe lists sampled with linear interpolation at render
 * time. Meshes are shared templates; per-frame vertex scratch is owned by
 * the anim (allocated once, reused every frame — no hot-path allocs).
 */

#include "wbus/wbus_anim.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WB_ANIM_MAX_KEYS 64
#define WB_ANIM_MAX_OBJS 32

typedef struct {
    double t;
    float px, py, pz;
    float rx, ry, rz;
    float scale;
} wb_anim_keyframe;

typedef struct {
    const wb_mesh *mesh;
    uint8_t r, g, b;
    wb_anim_keyframe keys[WB_ANIM_MAX_KEYS];
    int nkeys;
    /* baked scratch for this object's transformed copy */
    wb_rast_vertex *xverts;
    int             xcap;
} wb_anim_obj;

struct wb_anim {
    int w, h;
    wb_anim_obj objs[WB_ANIM_MAX_OBJS];
    int nobjs;

    /* single combined draw list rebuilt each frame */
    wb_rast_vertex *draw_verts;
    wb_rast_tri    *draw_tris;
    int cap_verts, cap_tris;
};

wb_anim *wb_anim_create(int width, int height) {
    if (width <= 0 || height <= 0) return NULL;
    wb_anim *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->w = width; a->h = height;
    return a;
}

void wb_anim_free(wb_anim *a) {
    if (!a) return;
    for (int i = 0; i < a->nobjs; i++) free(a->objs[i].xverts);
    free(a->draw_verts); free(a->draw_tris);
    free(a);
}

int wb_anim_add_object(wb_anim *a, const wb_mesh *m,
                       uint8_t r, uint8_t g, uint8_t b) {
    if (!a || !m || a->nobjs >= WB_ANIM_MAX_OBJS) return -1;
    wb_anim_obj *o = &a->objs[a->nobjs];
    memset(o, 0, sizeof(*o));
    o->mesh = m;
    o->r = r; o->g = g; o->b = b;
    return a->nobjs++;
}

static int keyframe_cmp(const void *pa, const void *pb) {
    const wb_anim_keyframe *x = pa, *y = pb;
    return (x->t > y->t) - (x->t < y->t);
}

int wb_anim_key(wb_anim *a, int obj, double t,
                float px, float py, float pz,
                float rx, float ry, float rz,
                float scale) {
    if (!a || obj < 0 || obj >= a->nobjs || t < 0 || scale <= 0) return -1;
    wb_anim_obj *o = &a->objs[obj];
    if (o->nkeys >= WB_ANIM_MAX_KEYS) return -1;
    wb_anim_keyframe *k = &o->keys[o->nkeys++];
    k->t = t; k->px = px; k->py = py; k->pz = pz;
    k->rx = rx; k->ry = ry; k->rz = rz; k->scale = scale;
    qsort(o->keys, o->nkeys, sizeof(wb_anim_keyframe), keyframe_cmp);
    return 0;
}

double wb_anim_duration(const wb_anim *a) {
    if (!a) return 0;
    double d = 0;
    for (int i = 0; i < a->nobjs; i++)
        for (int k = 0; k < a->objs[i].nkeys; k++)
            if (a->objs[i].keys[k].t > d) d = a->objs[i].keys[k].t;
    return d;
}

int wb_anim_object_count(const wb_anim *a) { return a ? a->nobjs : 0; }

/* sample object channels at t into out (clamped hold outside key range) */
static void sample_obj(const wb_anim_obj *o, double t, wb_anim_keyframe *out) {
    if (o->nkeys == 0) {
        out->px = out->py = out->pz = 0;
        out->rx = out->ry = out->rz = 0;
        out->scale = 1;
        return;
    }
    if (t <= o->keys[0].t) { *out = o->keys[0]; return; }
    if (t >= o->keys[o->nkeys-1].t) { *out = o->keys[o->nkeys-1]; return; }
    for (int k = 0; k + 1 < o->nkeys; k++) {
        const wb_anim_keyframe *k0 = &o->keys[k], *k1 = &o->keys[k+1];
        if (t >= k0->t && t <= k1->t) {
            float f = (float)((t - k0->t) / (k1->t - k0->t));
            out->t = t;
            out->px = k0->px + (k1->px-k0->px)*f;
            out->py = k0->py + (k1->py-k0->py)*f;
            out->pz = k0->pz + (k1->pz-k0->pz)*f;
            out->rx = k0->rx + (k1->rx-k0->rx)*f;
            out->ry = k0->ry + (k1->ry-k0->ry)*f;
            out->rz = k0->rz + (k1->rz-k0->rz)*f;
            out->scale = k0->scale + (k1->scale-k0->scale)*f;
            return;
        }
    }
    *out = o->keys[o->nkeys-1];
}

/* bake one object's mesh through its sampled transform into xverts */
static void bake_obj(wb_anim_obj *o, const wb_anim_keyframe *kf) {
    int nv = wb_mesh_vert_count(o->mesh);
    if (nv > o->xcap) {
        wb_rast_vertex *nx = realloc(o->xverts, (size_t)nv * sizeof(*nx));
        if (!nx) return;
        o->xverts = nx; o->xcap = nv;
    }
    const wb_rast_vertex *src = wb_mesh_vert_src(o->mesh);
    float cx = cosf(kf->rx), sxn = sinf(kf->rx);
    float cy = cosf(kf->ry), syn = sinf(kf->ry);
    float cz = cosf(kf->rz), szn = sinf(kf->rz);
    for (int i = 0; i < nv; i++) {
        float x = src[i].x * kf->scale, y = src[i].y * kf->scale, z = src[i].z * kf->scale;
        /* Rx */
        float y1 = y*cx - z*sxn, z1 = y*sxn + z*cx;
        /* Ry */
        float x2 = x*cy + z1*syn, z2 = -x*syn + z1*cy;
        /* Rz */
        float x3 = x2*cz - y1*szn, y3 = x2*szn + y1*cz;
        o->xverts[i].x = x3 + kf->px;
        o->xverts[i].y = y3 + kf->py;
        o->xverts[i].z = z2 + kf->pz;
    }
}

/* growable draw-list append helpers */
static void draw_vert(wb_anim *a, float x, float y, float z, int *n) {
    if (*n >= a->cap_verts) {
        int nc = a->cap_verts ? a->cap_verts*2 : 256;
        wb_rast_vertex *nv = realloc(a->draw_verts, (size_t)nc*sizeof(*nv));
        if (!nv) return;
        a->draw_verts = nv; a->cap_verts = nc;
    }
    a->draw_verts[*n] = (wb_rast_vertex){x,y,z}; (*n)++;
}
static void draw_tri(wb_anim *a, int v0,int v1,int v2,
                     uint8_t r,uint8_t g,uint8_t b, int *n) {
    if (*n >= a->cap_tris) {
        int nc = a->cap_tris ? a->cap_tris*2 : 512;
        wb_rast_tri *nt = realloc(a->draw_tris, (size_t)nc*sizeof(*nt));
        if (!nt) return;
        a->draw_tris = nt; a->cap_tris = nc;
    }
    a->draw_tris[*n] = (wb_rast_tri){v0,v1,v2,r,g,b}; (*n)++;
}

void wb_anim_render_frame(wb_anim *a, double t, uint8_t *out_rgba) {
    if (!a || !out_rgba) return;
    memset(out_rgba, 0, (size_t)a->w * a->h * 4);

    int nv = 0, nt = 0;
    for (int i = 0; i < a->nobjs; i++) {
        wb_anim_obj *o = &a->objs[i];
        if (o->nkeys == 0) continue;   /* no keys: object not on stage yet */
        wb_anim_keyframe kf;
        sample_obj(o, t, &kf);
        bake_obj(o, &kf);
        int base = nv;
        int mv = wb_mesh_vert_count(o->mesh);
        int mt = wb_mesh_tri_count(o->mesh);
        const wb_rast_tri *src_tris = wb_mesh_tri_src(o->mesh);
        for (int v = 0; v < mv; v++)
            draw_vert(a, o->xverts[v].x, o->xverts[v].y, o->xverts[v].z, &nv);
        for (int q = 0; q < mt; q++)
            draw_tri(a, base+src_tris[q].v0, base+src_tris[q].v1, base+src_tris[q].v2,
                     o->r, o->g, o->b, &nt);
    }

    if (nt == 0) return;
    wb_rast_ctx *r = wb_rast_create(a->w, a->h);
    if (!r) return;
    wb_rast_set_scene(r, a->draw_verts, nv, a->draw_tris, nt);
    wb_rast_set_camera(r, 0.35f, 0.55f, 0, 9.0f, 0);
    wb_rast_render(r, out_rgba);
    wb_rast_destroy(r);
}
