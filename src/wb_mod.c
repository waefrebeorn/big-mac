/* wb_mod.c — non-destructive modifier stack (R055b). */

#include "wbus/wbus_mod.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef WB_MOD_PI
#define WB_MOD_PI 3.14159265358979
#endif

typedef struct wb_modifier {
    wb_mod_type type;
    /* union-ish params */
    int   count, axis;
    float dx, dy, dz;
    float amp, waves;
    float radians;
    float thickness;
} wb_modifier;

struct wb_modstack {
    wb_modifier mods[16];
    int nmods;
};

wb_modstack *wb_mod_create(void) { return calloc(1, sizeof(wb_modstack)); }
void wb_mod_free(wb_modstack *ms) { free(ms); }
int  wb_mod_count(const wb_modstack *ms) { return ms ? ms->nmods : 0; }

static wb_modifier *push(wb_modstack *ms) {
    if (!ms || ms->nmods >= 16) return NULL;
    return &ms->mods[ms->nmods++];
}
int wb_mod_add_array(wb_modstack *ms, int count, float dx, float dy, float dz) {
    wb_modifier *m = push(ms); if (!m) return -1;
    m->type = WB_MOD_ARRAY; m->count = count < 1 ? 1 : count;
    m->dx = dx; m->dy = dy; m->dz = dz; return ms->nmods - 1;
}
int wb_mod_add_mirror(wb_modstack *ms, int axis) {
    wb_modifier *m = push(ms); if (!m) return -1;
    m->type = WB_MOD_MIRROR; m->axis = axis < 0 ? 0 : (axis > 2 ? 2 : axis);
    return ms->nmods - 1;
}
int wb_mod_add_wave(wb_modstack *ms, float amp, float waves) {
    wb_modifier *m = push(ms); if (!m) return -1;
    m->type = WB_MOD_WAVE; m->amp = amp; m->waves = waves;
    return ms->nmods - 1;
}
int wb_mod_add_twist(wb_modstack *ms, float radians_total) {
    wb_modifier *m = push(ms); if (!m) return -1;
    m->type = WB_MOD_TWIST; m->radians = radians_total; return ms->nmods - 1;
}
int wb_mod_add_solidify(wb_modstack *ms, float thickness) {
    wb_modifier *m = push(ms); if (!m) return -1;
    m->type = WB_MOD_SOLIDIFY; m->thickness = thickness;
    return ms->nmods - 1;
}

/* ---- evaluation helpers (operate on growable copies) -------------------- */

typedef struct {
    wb_rast_vertex *v; int nv, capv;
    wb_rast_tri    *t; int nt, capt;
} buf;

static int buf_vert(buf *b, float x, float y, float z) {
    if (b->nv >= b->capv) {
        int nc = b->capv ? b->capv*2 : 64;
        wb_rast_vertex *p = realloc(b->v, (size_t)nc*sizeof(*p));
        if (!p) return -1;
        b->v = p; b->capv = nc;
    }
    b->v[b->nv].x=x; b->v[b->nv].y=y; b->v[b->nv].z=z;
    return b->nv++;
}
static void buf_tri(buf *b, int a,int c,int d, uint8_t r,uint8_t g,uint8_t bl){
    if (b->nt >= b->capt) {
        int nc = b->capt ? b->capt*2 : 128;
        wb_rast_tri *p = realloc(b->t, (size_t)nc*sizeof(*p));
        if (!p) return;
        b->t = p; b->capt = nc;
    }
    b->t[b->nt].v0=a; b->t[b->nt].v1=c; b->t[b->nt].v2=d;
    b->t[b->nt].r=r; b->t[b->nt].g=g; b->t[b->nt].b=bl;
    b->nt++;
}

static void load_mesh(buf *b, const wb_mesh *m) {
    int nv = wb_mesh_vert_count(m), nt = wb_mesh_tri_count(m);
    const wb_rast_vertex *vs = wb_mesh_vert_src(m);
    const wb_rast_tri    *ts = wb_mesh_tri_src(m);
    for (int i = 0; i < nv; i++) buf_vert(b, vs[i].x, vs[i].y, vs[i].z);
    for (int i = 0; i < nt; i++)
        buf_tri(b, ts[i].v0, ts[i].v1, ts[i].v2, ts[i].r, ts[i].g, ts[i].b);
}

wb_mesh *wb_mod_apply(const wb_mesh *src, const wb_modstack *ms) {
    if (!src || !ms) return NULL;
    buf cur = {0};
    load_mesh(&cur, src);

    for (int mi = 0; mi < ms->nmods; mi++) {
        const wb_modifier *mod = &ms->mods[mi];
        switch (mod->type) {

        case WB_MOD_ARRAY: {
            buf next = {0};
            for (int c = 0; c < mod->count; c++) {
                float ox = mod->dx*c, oy = mod->dy*c, oz = mod->dz*c;
                int base = next.nv;
                for (int i = 0; i < cur.nv; i++)
                    buf_vert(&next, cur.v[i].x+ox, cur.v[i].y+oy, cur.v[i].z+oz);
                for (int i = 0; i < cur.nt; i++) {
                    wb_rast_tri *t=&cur.t[i];
                    buf_tri(&next, base+t->v0, base+t->v1, base+t->v2,
                            t->r,t->g,t->b);
                }
            }
            free(cur.v); free(cur.t);
            cur = next;
            break;
        }

        case WB_MOD_MIRROR: {
            buf next = {0};
            int base = cur.nv;
            for (int i = 0; i < cur.nv; i++) buf_vert(&next, cur.v[i].x, cur.v[i].y, cur.v[i].z);
            for (int i = 0; i < cur.nv; i++) {
                wb_rast_vertex v = cur.v[i];
                if (mod->axis==0) v.x=-v.x; else if (mod->axis==1) v.y=-v.y; else v.z=-v.z;
                buf_vert(&next, v.x, v.y, v.z);
            }
            for (int i = 0; i < cur.nt; i++) {
                wb_rast_tri *t=&cur.t[i];
                buf_tri(&next, t->v0, t->v1, t->v2, t->r,t->g,t->b);
                /* reversed winding on the mirrored copy */
                buf_tri(&next, base+t->v0, base+t->v2, base+t->v1, t->r,t->g,t->b);
            }
            free(cur.v); free(cur.t);
            cur = next;
            break;
        }

        case WB_MOD_WAVE: {
            for (int i = 0; i < cur.nv; i++) {
                float x = cur.v[i].x;
                cur.v[i].y += mod->amp * sinf(x / 4.0f * mod->waves * 2.0f * WB_MOD_PI);
            }
            break;
        }

        case WB_MOD_TWIST: {
            /* twist proportional to height y over [-2,2] */
            for (int i = 0; i < cur.nv; i++) {
                float x = cur.v[i].x, y = cur.v[i].y, z = cur.v[i].z;
                float f = (y + 2.0f) / 4.0f;           /* 0..1 */
                float ang = mod->radians * f;
                float ca = cosf(ang), sa = sinf(ang);
                cur.v[i].x = x*ca + z*sa;
                cur.v[i].z = -x*sa + z*ca;
            }
            break;
        }

        case WB_MOD_SOLIDIFY: {
            /* duplicate the surface offset by +/-thickness/2 along its
             * average normal direction approximated as ±Y (plane shells),
             * plus rim quads along boundary edges. V1: offset both ways,
             * skip rim (closed meshes don't need it). */
            buf next = {0};
            float h = mod->thickness * 0.5f;
            for (int i = 0; i < cur.nv; i++) buf_vert(&next, cur.v[i].x, cur.v[i].y-h, cur.v[i].z);
            for (int i = 0; i < cur.nv; i++) buf_vert(&next, cur.v[i].x, cur.v[i].y+h, cur.v[i].z);
            for (int i = 0; i < cur.nt; i++) {
                wb_rast_tri *t=&cur.t[i];
                buf_tri(&next, t->v0, t->v1, t->v2, t->r,t->g,t->b);
                int n = cur.nv;
                buf_tri(&next, n+t->v0, n+t->v2, n+t->v1,
                        (uint8_t)(t->r*8/10),(uint8_t)(t->g*8/10),(uint8_t)(t->b*8/10));
            }
            free(cur.v); free(cur.t);
            cur = next;
            break;
        }
        }
    }

extern wb_mesh *wb_mesh_build(const wb_rast_vertex *verts, int nverts,
                                  const wb_rast_tri *tris, int ntris);
    wb_mesh *out = wb_mesh_build(cur.v, cur.nv, cur.t, cur.nt);
    free(cur.v); free(cur.t);
    return out;
}
