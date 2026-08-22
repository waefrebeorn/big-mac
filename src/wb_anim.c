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

#ifndef WB_ANIM_PI
#define WB_ANIM_PI 3.14159265358979
#endif

typedef struct wb_anim_keyframe {
    double t;
    float px, py, pz;
    float rx, ry, rz;
    float scale;
    int   ease;              /* R055c: easing into this key */
} wb_anim_keyframe;

typedef struct {
    const wb_mesh *mesh;
    uint8_t r, g, b;
    int parent;              /* R055c: -1 none */
    wb_anim_keyframe keys[WB_ANIM_MAX_KEYS];
    int nkeys;
    /* baked scratch for this object's transformed copy */
    wb_rast_vertex *xverts;
    int             xcap;
} wb_anim_obj;

struct wb_anim {
    int w, h;
    /* R055c: camera track */
    int   ncam_keys;
    wb_anim_keyframe cam_keys[WB_ANIM_MAX_KEYS];
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
    o->parent = -1;
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

int wb_anim_key_ease(wb_anim *a, int obj, double t,
                     float px, float py, float pz,
                     float rx, float ry, float rz,
                     float scale, int ease) {
    int r = wb_anim_key(a, obj, t, px, py, pz, rx, ry, rz, scale);
    if (r == 0) {
        /* the last-inserted key is this one (sorted to its slot) */
        for (int k = 0; k < a->objs[obj].nkeys; k++)
            if (fabs(a->objs[obj].keys[k].t - t) < 1e-9)
                a->objs[obj].keys[k].ease = ease;
    }
    return r;
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

/* R055c: easing curves — f in [0,1] -> eased f */
static float ease_apply(int kind, float f) {
    if (f <= 0) return 0;
    if (f >= 1) return 1;
    switch (kind) {
    case 1:  return f*f*(3.0f-2.0f*f);                    /* smoothstep   */
    case 4:  return f*f;                                   /* ease-in quad */
    case 2: {                                              /* bounce-out   */
        const float n1=7.5625f, d1=2.75f;
        if (f < 1/d1) return n1*f*f;
        if (f < 2/d1) { f-=1.5f/d1; return n1*f*f+0.75f; }
        if (f < 2.5f/d1){ f-=2.25f/d1; return n1*f*f+0.9375f; }
        f-=2.625f/d1; return n1*f*f+0.984375f;
    }
    case 3: {                                              /* elastic out  */
        float p = 0.3f;
        return powf(2,-10*f)*sinf((f-p/4)*(2*WB_ANIM_PI)/p)+1;
    }
    default: return f;
    }
}

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
            float raw = (float)((t - k0->t) / (k1->t - k0->t));
            float f = ease_apply(k1->ease, raw);   /* R055c: eased segment */
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

/* R055c: parenting — child transform composes after parent's sample. */
int wb_anim_parent(wb_anim *a, int child, int parent) {
    if (!a || child < 0 || child >= a->nobjs) return -1;
    if (parent >= child) return -1;          /* parent must precede child */
    a->objs[child].parent = parent;
    return 0;
}

int wb_anim_set_camera(wb_anim *a, float rx, float ry, float dist) {
    return wb_anim_key_camera(a, 0.0, rx, ry, dist);
}

int wb_anim_key_camera(wb_anim *a, double t, float rx, float ry, float dist) {
    if (!a || t < 0 || a->ncam_keys >= WB_ANIM_MAX_KEYS || dist <= 0) return -1;
    wb_anim_keyframe *k = &a->cam_keys[a->ncam_keys++];
    memset(k, 0, sizeof(*k));
    k->t = t; k->rx = rx; k->ry = ry; k->pz = dist; k->scale = 1;
    /* sort by time */
    for (int i = a->ncam_keys-1; i > 0; i--)
        if (a->cam_keys[i].t < a->cam_keys[i-1].t) {
            wb_anim_keyframe tmp = a->cam_keys[i];
            a->cam_keys[i] = a->cam_keys[i-1];
            a->cam_keys[i-1] = tmp;
        }
    return 0;
}

void wb_anim_render_frame(wb_anim *a, double t, uint8_t *out_rgba) {
    if (!a || !out_rgba) return;
    memset(out_rgba, 0, (size_t)a->w * a->h * 4);

    int nv = 0, nt = 0;

    /* R055c: sample camera track */
    float cam_rx = 0.35f, cam_ry = 0.55f, cam_dist = 9.0f;
    if (a->ncam_keys > 0) {
        wb_anim_keyframe ck;
        const wb_anim_obj fake = { .keys={0}, .nkeys=0 };
        (void)fake;
        /* reuse object sampler on the camera key list via local copy */
        wb_anim_obj co; memset(&co, 0, sizeof(co));
        memcpy(co.keys, a->cam_keys, sizeof(a->cam_keys));
        co.nkeys = a->ncam_keys;
        sample_obj(&co, t, &ck);
        cam_rx = ck.rx; cam_ry = ck.ry; cam_dist = ck.pz;
    }

    for (int i = 0; i < a->nobjs; i++) {
        wb_anim_obj *o = &a->objs[i];
        if (o->nkeys == 0) continue;   /* no keys: object not on stage yet */
        wb_anim_keyframe kf;
        sample_obj(o, t, &kf);
        /* R055c: additively inherit parent's sampled translation */
        if (o->parent >= 0) {
            wb_anim_keyframe pkf;
            sample_obj(&a->objs[o->parent], t, &pkf);
            kf.px += pkf.px; kf.py += pkf.py; kf.pz += pkf.pz;
        }
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
    wb_rast_set_camera(r, cam_rx, cam_ry, 0, cam_dist, 0);
    wb_rast_render(r, out_rgba);
    wb_rast_destroy(r);
}
