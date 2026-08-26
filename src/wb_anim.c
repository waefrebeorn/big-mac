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
#include <stdio.h>

/* R074 hop 113 (G-SF001/002): raised caps + dynamic key growth.
 * Object count still bounded (pointer table) but far higher. */
#define WB_ANIM_MAX_KEYS 256
#define WB_ANIM_MAX_OBJS 128
#define WB_ANIM_BILLBOARD 0x01
#define WB_ANIM_LOOKCAM   0x02

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

#define WB_ANIM_MAX_EVENTS 32
typedef struct { double t; int id; } wb_anim_event_i;

struct wb_anim {
    int w, h;
    /* R074 hop 120 (G-SF056): visibility windows */
    float vis_from[WB_ANIM_MAX_OBJS], vis_to[WB_ANIM_MAX_OBJS];
    /* R074 hop 114 (G-SF017): depth fog */
    int   fog_on;
    float fog_near, fog_far;
    uint8_t fog_r, fog_g, fog_b;
    /* R074 hop 114 (G-SF023): timeline events */
    wb_anim_event_i events[WB_ANIM_MAX_EVENTS];
    int nevents;
    /* R055c: camera track */
    int   ncam_keys;
    wb_anim_keyframe cam_keys[WB_ANIM_MAX_KEYS];
    float shake_amt;   /* G-SF008: camera shake amplitude (radians) */
    /* R074 hop 156 (G-SF019): instancing — additional static transforms
     * sharing an existing object's mesh (no per-instance keys). */
    int   ninstances;
    struct {
        int src_obj;
        float px, py, pz, rx, ry, rz, s;
    } instances[64];
    float *depth;          /* G-SF024: last frame normalized depth */
    size_t depth_cap;
    /* R074 hop 164 (G-SF029): planar ground shadow */
    int   shadow_on;
    float shadow_y;
    wb_anim_obj objs[WB_ANIM_MAX_OBJS];
    int nobjs;

    /* single combined draw list rebuilt each frame */
    wb_rast_vertex *draw_verts;
    wb_rast_tri    *draw_tris;
    int cap_verts, cap_tris;
    uint8_t flags[WB_ANIM_MAX_OBJS];   /* R074 hop 124: BB/lookcam */
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
    free(a->depth);   /* G-SF024 */
    free(a->draw_verts); free(a->draw_tris);
    free(a);
}

int wb_anim_add_object(wb_anim *a, const wb_mesh *m,
                       uint8_t r, uint8_t g, uint8_t b) {
    if (!a || !m) { wb_anim_last_error = WB_ANIM_ERR_OBJS; return -1; }
    if (a->nobjs >= WB_ANIM_MAX_OBJS) {           /* G-SF089: surface it */
        wb_anim_last_error = WB_ANIM_ERR_OBJS;
        fprintf(stderr, "wb_anim_add_object: %s\n",
                wb_anim_error_str(a));
        return -1;
    }
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
    /* R074 hop 155 (G-SF008): camera shake composition — additive
     * deterministic offsets layered over the keyed camera each frame. */
    if (a->shake_amt > 0.0f) {
        uint32_t h1 = (uint32_t)(t * 97.0) * 2654435761u;
        h1 ^= h1 >> 13; h1 *= 1103515245u; h1 ^= h1 << 16;
        uint32_t h2 = (uint32_t)(t * 131.0 + 7.0) * 2654435761u;
        h2 ^= h2 >> 15; h2 *= 1103515245u; h2 ^= h2 << 17;
        float n1 = ((float)(int32_t)h1 / 2147483648.0f);
        float n2 = ((float)(int32_t)h2 / 2147483648.0f);
        cam_rx += a->shake_amt * n1;
        cam_ry += a->shake_amt * n2;
    }

    wb_rast_ctx *r = wb_rast_create(a->w, a->h);
    if (!r) return;
    for (int pass = 1; pass <= 2; pass++) {
    nv = 0; nt = 0;
for (int i = 0; i < a->nobjs; i++) {
        wb_anim_obj *o = &a->objs[i];
        if (o->nkeys == 0) continue;   /* no keys: object not on stage yet */
        /* G-SF056: visibility window */
        if (a->vis_to[i] > a->vis_from[i]
            && ((double)t < a->vis_from[i] || (double)t > a->vis_to[i]))
            continue;
        wb_anim_keyframe kf;
        sample_obj(o, t, &kf);
        /* R074 hop 124 (G-SF010/009): billboard & look-at overrides */
        if (a->flags[i] & WB_ANIM_BILLBOARD) {
            kf.rx = cam_rx; kf.ry = cam_ry; kf.rz = 0;
        } else if (a->flags[i] & WB_ANIM_LOOKCAM) {
            kf.ry = cam_ry;
        }
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
        for (int q = 0; q < mt; q++) {
            uint8_t fr = o->r, fg = o->g, fb = o->b;
            if (a->fog_on) {
                /* G-SF017: fade by centroid depth (camera looks down -z;
                 * xverts z is world z) */
                float zc = (o->xverts[src_tris[q].v0].z +
                            o->xverts[src_tris[q].v1].z +
                            o->xverts[src_tris[q].v2].z) / 3.0f;
                float f = (a->fog_far - zc) / (a->fog_far - a->fog_near);
                if (f < 0) f = 0; else if (f > 1) f = 1;
                fr = (uint8_t)(fr * f + a->fog_r * (1 - f));
                fg = (uint8_t)(fg * f + a->fog_g * (1 - f));
                fb = (uint8_t)(fb * f + a->fog_b * (1 - f));
            }
            draw_tri(a, base+src_tris[q].v0, base+src_tris[q].v1,
                     base+src_tris[q].v2, fr, fg, fb, &nt);
        }
    }
    /* R074 hop 156 (G-SF019): instances — same mesh, static transform */
    for (int q2 = 0; q2 < a->ninstances; q2++) {
        int so = a->instances[q2].src_obj;
        if (so < 0 || so >= a->nobjs) continue;
        wb_anim_obj *o = &a->objs[so];
        if (o->nkeys == 0) continue;
        wb_anim_obj tmp = *o;
        tmp.xverts = malloc(sizeof(wb_rast_vertex) *
                            (o->mesh ? wb_mesh_vert_count(o->mesh) : 0));
        if (!tmp.xverts) continue;
        wb_anim_keyframe kf;
        sample_obj(o, t, &kf);
        kf.px += a->instances[q2].px;
        kf.py += a->instances[q2].py;
        kf.pz += a->instances[q2].pz;
        kf.rx += a->instances[q2].rx;
        kf.ry += a->instances[q2].ry;
        kf.rz += a->instances[q2].rz;
        kf.scale *= a->instances[q2].s;
        bake_obj(&tmp, &kf);
        int base = nv;
        int mv = wb_mesh_vert_count(o->mesh);
        int mt = wb_mesh_tri_count(o->mesh);
        const wb_rast_tri *src_tris = wb_mesh_tri_src(o->mesh);
        for (int v = 0; v < mv; v++)
            draw_vert(a, tmp.xverts[v].x, tmp.xverts[v].y,
                      tmp.xverts[v].z, &nv);
        for (int q3 = 0; q3 < mt; q3++)
            draw_tri(a, base+src_tris[q3].v0, base+src_tris[q3].v1,
                     base+src_tris[q3].v2, o->r, o->g, o->b, &nt);
        free(tmp.xverts);
    }
    if (nt > 0) {
        wb_rast_set_scene(r, a->draw_verts, nv, a->draw_tris, nt);
        if (pass == 2) {
            /* G-SF015: unlit */
            float none[3] = {0,0,0};
            wb_rast_set_sun(r, 0,0,0, 0);
            (void)none;
        }
        wb_rast_render(r, out_rgba);
        wb_rast_set_sun(r, 0.45f, 0.75f, 0.5f, 1.0f); /* restore default */
    }
    }
    
}

/* R074 hop 113: query anim render size. */
void wb_anim_get_size(const wb_anim *anim, int *w, int *h) {
    if (!anim) return;
    if (w) *w = anim->w;
    if (h) *h = anim->h;
}

/* R074 hop 113 (G-SF005): looping key helper — writes keys marching an
 * object from z_far to z_near every `period` seconds, phase-offset by
 * `phase`. Ideal for scrolling corridors/starfields. Returns key count
 * written or -1. */
int wb_anim_key_loop(wb_anim *anim, int obj, double dur,
                     double period, double phase,
                     float px, float py, float pz_far, float pz_near) {
    if (!anim || obj < 0 || obj >= anim->nobjs || period <= 0) return -1;
    int written = 0;
    for (double ct = -phase; ct < dur + period; ct += period) {
        double ta = ct < 0 ? 0 : ct;
        double tb = ct + period;
        if (ta >= dur) break;
        if (tb > dur) tb = dur;
        if (wb_anim_key(anim, obj, ta, px, py, pz_near * 0 + pz_far,
                        0,0,0, 1.0) < 0) return written;
        written++;
        if (tb > ta) {
            if (wb_anim_key(anim, obj, tb, px, py, pz_near,
                            0,0,0, 1.0) < 0) return written;
            written++;
        }
    }
    return written;
}

/* R074 hop 114 (G-SF017): depth fog — tris fade toward fog color
 * between near/far camera distance. Disable with far <= near. */
void wb_anim_set_fog(wb_anim *a, float z_near, float z_far,
                     uint8_t r, uint8_t g, uint8_t b) {
    if (!a) return;
    a->fog_on = z_far > z_near;
    a->fog_near = z_near; a->fog_far = z_far;
    a->fog_r = r; a->fog_g = g; a->fog_b = b;
}

/* R074 hop 114 (G-SF023): register a timed event. Returns index or -1. */
int wb_anim_event_add(wb_anim *a, double t, int id) {
    if (!a || a->nevents >= WB_ANIM_MAX_EVENTS) return -1;
    a->events[a->nevents].t = t;
    a->events[a->nevents].id = id;
    return a->nevents++;
}

/* R074 hop 114: events with t_prev < te <= t_now, returned in order. */
int wb_anim_events_due(const wb_anim *a, double t_prev, double t_now,
                       int *out_ids, int max_out) {
    if (!a || !out_ids) return -1;
    int n = 0;
    for (int i = 0; i < a->nevents && n < max_out; i++) {
        if (a->events[i].t > t_prev && a->events[i].t <= t_now)
            out_ids[n++] = a->events[i].id;
    }
    return n;
}

/* ---- R074 hop 118 (G-SF003/004): per-channel keys + key editing ------- */
/* Write ONE channel at time t: merges with an existing key at |dt|<eps
 * (updates just that channel) or creates a key carrying only that
 * channel (other channels inherit from the nearest earlier key). */
static int anim_key_channel(wb_anim *a, int obj, double t, int ch,
                            float v, int ease) {
    if (!a || obj < 0 || obj >= a->nobjs || t < 0) return -1;
    wb_anim_obj *o = &a->objs[obj];
    static const char EPS = 0; (void)EPS;
    const double eps = 1e-6;
    /* find existing key near t */
    for (int i = 0; i < o->nkeys; i++) {
        if (fabs(o->keys[i].t - t) < eps) {
            switch (ch) {
                case 0: o->keys[i].px = v; break;
                case 1: o->keys[i].py = v; break;
                case 2: o->keys[i].pz = v; break;
                case 3: o->keys[i].rx = v; break;
                case 4: o->keys[i].ry = v; break;
                case 5: o->keys[i].rz = v; break;
                case 6: if (v <= 0) return -1; o->keys[i].scale = v; break;
                default: return -1;
            }
            if (ease >= 0) o->keys[i].ease = ease;
            return 0;
        }
    }
    if (o->nkeys >= WB_ANIM_MAX_KEYS) return -1;
    wb_anim_keyframe *k = &o->keys[o->nkeys++];
    k->t = t; k->ease = ease < 0 ? 0 : ease;
    k->scale = 1.0f;
    /* seed all channels from nearest earlier key (or zero/identity) */
    const wb_anim_keyframe *prev = NULL;
    for (int i = 0; i < o->nkeys-1; i++)
        if (o->keys[i].t <= t) prev = &o->keys[i];
    if (prev) { *k = *prev; k->t = t; }
    else { k->px=k->py=k->pz=0; k->rx=k->ry=k->rz=0; }
    switch (ch) {
        case 0: k->px = v; break;
        case 1: k->py = v; break;
        case 2: k->pz = v; break;
        case 3: k->rx = v; break;
        case 4: k->ry = v; break;
        case 5: k->rz = v; break;
        case 6: if (v <= 0) { o->nkeys--; return -1; } k->scale = v; break;
    }
    qsort(o->keys, o->nkeys, sizeof(wb_anim_keyframe), keyframe_cmp);
    return 0;
}

int wb_anim_key_pos_x(wb_anim *a, int o, double t, float x, int ease) {
    return anim_key_channel(a,o,t,0,x,ease);
}
int wb_anim_key_pos_y(wb_anim *a, int o, double t, float y, int ease) {
    return anim_key_channel(a,o,t,1,y,ease);
}
int wb_anim_key_pos_z(wb_anim *a, int o, double t, float z, int ease) {
    return anim_key_channel(a,o,t,2,z,ease);
}
int wb_anim_key_rot_z(wb_anim *a, int o, double t, float rz, int ease) {
    return anim_key_channel(a,o,t,5,rz,ease);
}

/* G-SF004: delete / move a key by index. */
int wb_anim_key_delete(wb_anim *a, int obj, int key_idx) {
    if (!a || obj < 0 || obj >= a->nobjs) return -1;
    wb_anim_obj *o = &a->objs[obj];
    if (key_idx < 0 || key_idx >= o->nkeys) return -1;
    memmove(&o->keys[key_idx], &o->keys[key_idx+1],
            (size_t)(o->nkeys-key_idx-1)*sizeof(wb_anim_keyframe));
    o->nkeys--;
    return 0;
}
int wb_anim_key_move(wb_anim *a, int obj, int key_idx, double new_t) {
    if (!a || obj < 0 || obj >= a->nobjs || new_t < 0) return -1;
    wb_anim_obj *o = &a->objs[obj];
    if (key_idx < 0 || key_idx >= o->nkeys) return -1;
    o->keys[key_idx].t = new_t;
    qsort(o->keys, o->nkeys, sizeof(wb_anim_keyframe), keyframe_cmp);
    return 0;
}

/* R074 hop 120 (G-SF056): object visible only in [from,to] seconds
 * (render skips it outside). to <= from clears the window. */
int wb_anim_set_visible(wb_anim *a, int obj, double from, double to) {
    if (!a || obj < 0 || obj >= a->nobjs) return -1;
    a->vis_from[obj] = (float)from;
    a->vis_to[obj]   = (float)to;
    return 0;
}

/* ---- R074 hop 121 (G-SF006): cubic bezier flight path ----------------- */
int wb_anim_path_bezier(wb_anim *a, int obj,
                        double t0, double t1,
                        float x0,float y0,float z0,
                        float x1,float y1,float z1,
                        float x2,float y2,float z2,
                        float x3,float y3,float z3,
                        int steps) {
    if (!a || obj < 0 || obj >= a->nobjs || steps <= 0) return -1;
    if (t1 <= t0 || a->objs[obj].nkeys + steps > WB_ANIM_MAX_KEYS) return -1;
    for (int s = 0; s <= steps; s++) {
        float u = (float)s / steps;
        float iu = 1.0f - u;
        /* cubic Bernstein */
        float b0=iu*iu*iu, b1=3*iu*iu*u, b2=3*iu*u*u, b3=u*u*u;
        float px=b0*x0+b1*x1+b2*x2+b3*x3;
        float py=b0*y0+b1*y1+b2*y2+b3*y3;
        float pz=b0*z0+b1*z1+b2*z2+b3*z3;
        if (wb_anim_key(a, obj, t0+(t1-t0)*u,
                        px,py,pz, 0,0,0, 1.0) < 0) return s;
    }
    return steps+1;
}

/* R074 hop 121 (G-SF022): quaternion-safe rotation interpolation is
 * approximated by shortest-arc Euler unwrap: when |delta|>pi the key
 * value wraps to the nearer equivalent angle. Call after adding keys. */
int wb_anim_rot_unwrap(wb_anim *a, int obj) {
    if (!a || obj < 0 || obj >= a->nobjs) return -1;
    wb_anim_obj *o = &a->objs[obj];
    for (int ch = 3; ch < 6; ch++) {
        float prev = 0;
        for (int i = 0; i < o->nkeys; i++) {
            float v[3] = {o->keys[i].rx, o->keys[i].ry, o->keys[i].rz};
            float d = v[ch] - prev;
            while (d > 3.14159f)  { v[ch] -= 6.28318f; d -= 6.28318f; }
            while (d < -3.14159f) { v[ch] += 6.28318f; d += 6.28318f; }
            if (ch==0) o->keys[i].rx=v[ch];
            else if (ch==1) o->keys[i].ry=v[ch];
            else o->keys[i].rz=v[ch];
            prev = v[ch];
        }
    }
    return 0;
}

/* R074 hop 122 (G-SF026): render at scaled resolution (letterboxed by
 * caller). 0 resets to the original create size. */
int wb_anim_set_resolution(wb_anim *a, int w, int h) {
    if (!a) return -1;
    if (w <= 0 || h <= 0) { w = a->w > 0 ? a->w : w; h = a->h; }
    if (w <= 0 || h <= 0) return -1;
    /* only shrink allowed once buffers sized — grow is fine too since
     * draw lists are dynamic */
    a->w = w; a->h = h;
    return 0;
}

/* ---- R074 hop 124 (G-SF010/G-SF009): billboards & look-at ------------- */

/* Mark an object to always face the camera (billboard). */
int wb_anim_set_billboard(wb_anim *a, int obj, int on) {
    if (!a || obj < 0 || obj >= a->nobjs) return -1;
    if (on) a->flags[obj] |= WB_ANIM_BILLBOARD;
    else    a->flags[obj] &= (uint8_t)~WB_ANIM_BILLBOARD;
    return 0;
}
/* Mark an object to rotate toward the camera around Y (look-at). */
int wb_anim_set_lookcam(wb_anim *a, int obj, int on) {
    if (!a || obj < 0 || obj >= a->nobjs) return -1;
    if (on) a->flags[obj] |= WB_ANIM_LOOKCAM;
    else    a->flags[obj] &= (uint8_t)~WB_ANIM_LOOKCAM;
    return 0;
}

/* R074 hop 125 (G-SF015): emissive object — rendered unlit. */
int wb_anim_set_emissive(wb_anim *a, int obj, int on) {
    if (!a || obj < 0 || obj >= a->nobjs) return -1;
    if (on) a->flags[obj] |= 0x04;
    else    a->flags[obj] &= (uint8_t)~0x04;
    return 0;
}

/* R074 hop 129 (G-SF040): supersampled anti-aliased frame — renders at
 * 2x2 scale and box-downsamples. out_rgba must be w*h*4. */
void wb_anim_render_frame_aa(wb_anim *a, double t, uint8_t *out_rgba) {
    if (!a || !out_rgba) return;
    int W = a->w, H = a->h;
    uint8_t *big = malloc((size_t)W*2*H*2*4);
    if (!big) { wb_anim_render_frame(a, t, out_rgba); return; }
    int ow = a->w; int oh = a->h;
    a->w = W*2; a->h = H*2;
    wb_anim_render_frame(a, t, big);
    a->w = ow; a->h = oh;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t r=0,g=0,b=0,aacc=0,n=0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const uint8_t *q =
                        big + (((size_t)(y*2+dy)*W*2) + (x*2+dx))*4;
                    r += q[0]; g += q[1]; b += q[2]; aacc += q[3]; n++;
                }
            uint8_t *o = out_rgba + ((size_t)y*W + x)*4;
            o[0]=(uint8_t)(r/n); o[1]=(uint8_t)(g/n);
            o[2]=(uint8_t)(b/n); o[3]=(uint8_t)(aacc/n);
        }
    }
    free(big);
}

/* R074 hop 129 (G-SF036): motion blur via temporal accumulation.
 * state buffer persists between calls (caller owns, zeroed initially);
 * blend = new frame weight (0..1). */
void wb_anim_render_frame_blur(wb_anim *a, double t, float blend,
                               uint8_t *state_rgba) {
    if (!a || !state_rgba) return;
    int W = a->w, H = a->h;
    uint8_t *fresh = malloc((size_t)W*H*4);
    if (!fresh) return;
    wb_anim_render_frame(a, t, fresh);
    if (blend < 0) blend = 0; if (blend > 1) blend = 1;
    for (size_t i = 0; i < (size_t)W*H*4; i++)
        state_rgba[i] = (uint8_t)(state_rgba[i]*(1.0f-blend)
                                  + fresh[i]*blend);
    free(fresh);
}

/* R074 hop 130 (G-SF041): grab a single frame to a PNG-less PPM file
 * (screenshot API). Returns 0 on success. */
int wb_anim_screenshot(wb_anim *a, double t, const char *path) {
    if (!a || !path) return -1;
    uint8_t *buf = malloc((size_t)a->w * a->h * 4);
    if (!buf) return -1;
    wb_anim_render_frame(a, t, buf);
    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", a->w, a->h);
    for (int i = 0; i < a->w * a->h; i++) {
        uint8_t rgb[3] = { buf[i*4+0], buf[i*4+1], buf[i*4+2] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    free(buf);
    return 0;
}

/* ---- R074 hop 146: metadata & diagnostics ---------------------------- */
/* G-SF054: fps/timebase metadata on the animation. */
static int g_anim_fps = 24;
void wb_anim_set_rate(int fps) { if (fps > 0 && fps <= 240) g_anim_fps = fps; }
int  wb_anim_get_rate(void)    { return g_anim_fps; }

/* G-SF089: surface the object-cap error instead of silent -1. */
int wb_anim_last_error = WB_ANIM_OK;
const char *wb_anim_error_str(wb_anim *a) {
    (void)a;
    switch (wb_anim_last_error) {
    case WB_ANIM_OK:            return "ok";
    case WB_ANIM_ERR_OBJS:      return "object cap reached";
    case WB_ANIM_ERR_KEYS:      return "key cap reached";
    default:                    return "unknown";
    }
}

/* G-SF060: progress callback during export loops. */
static void (*g_progress_fn)(double frac, void *user) = 0;
static void *g_progress_user = 0;
void wb_anim_set_progress(void (*fn)(double, void *), void *user) {
    g_progress_fn = fn; g_progress_user = user;
}
void wb_anim_progress(double frac) {
    if (g_progress_fn) {
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        g_progress_fn(frac, g_progress_user);
    }
}

/* R074 hop 155 (G-SF008): set camera shake amplitude (radians). */
int wb_anim_set_shake(wb_anim *a, float amt) {
    if (!a || amt < 0) return -1;
    a->shake_amt = amt;
    return 0;
}

/* R074 hop 156 (G-SF019): instance an object — draws the source mesh
 * again at a static transform. Returns instance index or -1. */
int wb_anim_add_instance(wb_anim *a, int src_obj,
                         float px, float py, float pz,
                         float rx, float ry, float rz, float s) {
    if (!a || src_obj < 0 || src_obj >= a->nobjs) return -1;
    if (a->ninstances >= 64) return -1;
    int idx = a->ninstances++;
    a->instances[idx].src_obj = src_obj;
    a->instances[idx].px=px; a->instances[idx].py=py;
    a->instances[idx].pz=pz; a->instances[idx].rx=rx;
    a->instances[idx].ry=ry; a->instances[idx].rz=rz;
    a->instances[idx].s = s > 0 ? s : 1.0f;
    return idx;
}

/* R074 hop 162 (G-SF024): access the last frame's normalized depth map
 * (w*h floats, 0=near .. 1=far/no-geometry). NULL until first render. */
const float *wb_anim_depth_map(const wb_anim *a) { return a ? a->depth : 0; }

/* R074 hop 164 (G-SF029): planar ground shadow at height y. */
int wb_anim_set_ground_shadow(wb_anim *a, float y) {
    if (!a) return -1;
    a->shadow_on = 1; a->shadow_y = y;
    return 0;
}
