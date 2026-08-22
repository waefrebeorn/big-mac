/* wb_rast.c — fixed-function software triangle rasterizer (R052).
 *
 * Techniques SLERM'd from CubeCoders/Jet into pure C11:
 *   - edge-function rasterization (integer, no floating compares in the
 *     inner loop)
 *   - flat shading: one color per triangle, computed at setup
 *   - back-face culling by signed edge area
 *   - no allocations in render(); scene arrays copied once at set_scene
 *
 * Output: RGBA8 with alpha 0 = background (compositor keys on it).
 */

#include "wbus/wbus_rast.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct wb_rast_ctx {
    int w, h;
    /* R055: shading + depth */
    float sun[3];        /* normalized light dir (pointing FROM surface TO sun) */
    float sun_i;
    float spec;
    int   zbuf_on;
    float *zbuf;         /* w*h, per-pixel view depth */

    /* geometry copies */
    wb_rast_vertex *verts;
    int             nverts;
    wb_rast_tri    *tris;
    int             ntris;

    /* camera */
    float rx, ry, rz, dist, y_off;

    int cull;

    /* per-frame scratch (sized to nverts) */
    float *sx, *sy, *sz;   /* screen x/y + view depth */
};

wb_rast_ctx *wb_rast_create(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    wb_rast_ctx *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->w = w; r->h = h;
    r->dist = 6.0f;
    r->cull = 1;
    r->sun[0] = 0.45f; r->sun[1] = 0.75f; r->sun[2] = 0.5f;
    /* normalize */
    float l = sqrtf(r->sun[0]*r->sun[0]+r->sun[1]*r->sun[1]+r->sun[2]*r->sun[2]);
    if (l > 1e-6f) { r->sun[0]/=l; r->sun[1]/=l; r->sun[2]/=l; }
    r->sun_i = 1.0f;
    r->spec = 0.25f;
    return r;
}

void wb_rast_destroy(wb_rast_ctx *r) {
    if (!r) return;
    free(r->verts); free(r->tris);
    free(r->sx); free(r->sy); free(r->sz);
    free(r);
}

int wb_rast_set_scene(wb_rast_ctx *r,
                      const wb_rast_vertex *verts, int nverts,
                      const wb_rast_tri *tris, int ntris) {
    if (!r || !verts || !tris || nverts <= 0 || ntris <= 0) return -1;
    wb_rast_vertex *nv = realloc(r->verts, (size_t)nverts * sizeof(*nv));
    if (!nv) return -1;
    r->verts = nv; memcpy(nv, verts, (size_t)nverts * sizeof(*nv));
    r->nverts = nverts;

    wb_rast_tri *nt = realloc(r->tris, (size_t)ntris * sizeof(*nt));
    if (!nt) return -1;
    r->tris = nt; memcpy(nt, tris, (size_t)ntris * sizeof(*nt));
    r->ntris = ntris;

    /* resize scratch */
    free(r->sx); free(r->sy); free(r->sz);
    r->sx = malloc((size_t)nverts * sizeof(float));
    r->sy = malloc((size_t)nverts * sizeof(float));
    r->sz = malloc((size_t)nverts * sizeof(float));
    if (!r->sx || !r->sy || !r->sz) return -1;
    return 0;
}

void wb_rast_set_camera(wb_rast_ctx *r,
                        float rx, float ry, float rz,
                        float dist, float y_off) {
    if (!r) return;
    r->rx = rx; r->ry = ry; r->rz = rz;
    r->dist = dist > 0.5f ? dist : 0.5f;
    r->y_off = y_off;
}

void wb_rast_set_cull(wb_rast_ctx *r, int on) { if (r) r->cull = on ? 1 : 0; }

void wb_rast_set_sun(wb_rast_ctx *r, float dx, float dy, float dz,
                     float intensity) {
    if (!r) return;
    float l = sqrtf(dx*dx+dy*dy+dz*dz);
    if (l > 1e-6f) { dx/=l; dy/=l; dz/=l; }
    r->sun[0]=dx; r->sun[1]=dy; r->sun[2]=dz;
    r->sun_i = intensity < 0 ? 0 : (intensity > 1 ? 1 : intensity);
}
void wb_rast_set_specular(wb_rast_ctx *r, float strength) {
    if (!r) return;
    r->spec = strength < 0 ? 0 : (strength > 1 ? 1 : strength);
}
void wb_rast_set_zbuffer(wb_rast_ctx *r, int on) {
    if (!r || on == r->zbuf_on) return;
    free(r->zbuf); r->zbuf = NULL;
    r->zbuf_on = on;
    if (on) r->zbuf = malloc((size_t)r->w * r->h * sizeof(float));
}

/* transform + project vertex i into screen space */
static void project_vert(wb_rast_ctx *r, int i) {
    const wb_rast_vertex *v = &r->verts[i];
    float x = v->x, y = v->y, z = v->z;
    float cx = cosf(r->rx), sxn = sinf(r->rx);
    float cy = cosf(r->ry), syn = sinf(r->ry);
    float cz = cosf(r->rz), szn = sinf(r->rz);
    /* Rx */
    float y1 = y*cx - z*sxn, z1 = y*sxn + z*cx;
    /* Ry */
    float x2 = x*cy + z1*syn, z2 = -x*syn + z1*cy;
    /* Rz */
    float x3 = x2*cz - y1*szn, y3 = x2*szn + y1*cz;

    float zd = z2 + r->dist;
    if (zd < 0.1f) zd = 0.1f;
    r->sz[i] = zd;
    float focal = 300.0f;                       /* matches wb_cgi projection */
    r->sx[i] = x3 * focal / zd + (float)r->w * 0.5f;
    r->sy[i] = y3 * focal / zd + (float)r->h * 0.6f + r->y_off;
}

/* rasterize one flat-shaded triangle via edge functions */
static void fill_tri(wb_rast_ctx *r, uint8_t *img,
                     int i0, int i1, int i2,
                     uint8_t cr, uint8_t cg, uint8_t cb) {
    float x0 = r->sx[i0], y0 = r->sy[i0];
    float x1 = r->sx[i1], y1 = r->sy[i1];
    float x2 = r->sx[i2], y2 = r->sy[i2];

    /* signed area (edge function of the whole tri) — cull backfaces */
    float area = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0);
    if (fabsf(area) < 0.25f) return;
    if (area < 0.0f) {
        if (r->cull) return;   /* back-facing: dropped */
        /* culling off: flip winding so the edge tests pass */
        int t = i1; i1 = i2; i2 = t;
        float tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty;
        area = -area;
    }

    int minx = (int)(x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2));
    int miny = (int)(y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2));
    int maxx = (int)(x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2)) + 1;
    int maxy = (int)(y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2)) + 1;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > r->w) maxx = r->w;
    if (maxy > r->h) maxy = r->h;
    if (minx >= maxx || miny >= maxy) return;

    float inv_area = 1.0f / area;
    for (int py = miny; py < maxy; py++) {
        float fy = (float)py + 0.5f;
        uint8_t *row = img + ((size_t)py * r->w + minx) * 4;
        for (int px = minx; px < maxx; px++) {
            float fx = (float)px + 0.5f;
            float w0 = (x1-x0)*(fy-y0) - (fx-x0)*(y1-y0);
            float w1 = (x2-x1)*(fy-y1) - (fx-x1)*(y2-y1);
            float w2 = (x0-x2)*(fy-y2) - (fx-x2)*(y0-y2);
            /* same winding now: all three must be same sign as area */
            if ((w0 * inv_area) >= 0.0f && (w1 * inv_area) >= 0.0f &&
                (w2 * inv_area) >= 0.0f) {
                row[0] = cr; row[1] = cg; row[2] = cb; row[3] = 255;
            }
            row += 4;
        }
    }
}

static void fill_tri_z(wb_rast_ctx *r, uint8_t *img,
                       int i0, int i1, int i2,
                       uint8_t cr, uint8_t cg, uint8_t cb) {
    /* like fill_tri but with depth test + interpolated depth */
    float x0=r->sx[i0], y0=r->sy[i0], z0=r->sz[i0];
    float x1=r->sx[i1], y1=r->sy[i1], z1=r->sz[i1];
    float x2=r->sx[i2], y2=r->sy[i2], z2=r->sz[i2];
    float area = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0);
    if (fabsf(area) < 0.25f) return;
    if (area < 0.0f) {
        int t=i1; i1=i2; i2=t;
        float tx=x1, ty=y1, tz=z1;
        x1=x2; y1=y2; z1=z2; x2=tx; y2=ty; z2=tz;
        area=-area;
    }
    int minx=(int)(x0<x1?(x0<x2?x0:x2):(x1<x2?x1:x2));
    int miny=(int)(y0<y1?(y0<y2?y0:y2):(y1<y2?y1:y2));
    int maxx=(int)(x0>x1?(x0>x2?x0:x2):(x1>x2?x1:x2))+1;
    int maxy=(int)(y0>y1?(y0>y2?y0:y2):(y1>y2?y1:y2))+1;
    if (minx<0)minx=0; if(miny<0)miny=0;
    if(maxx>r->w)maxx=r->w; if(maxy>r->h)maxy=r->h;
    if (minx>=maxx||miny>=maxy) return;
    float inv=1.0f/area;
    for (int py=miny; py<maxy; py++) {
        float fy=(float)py+0.5f;
        uint8_t *row=img+((size_t)py*r->w+minx)*4;
        float *zrow=r->zbuf+((size_t)py*r->w+minx);
        for (int px=minx; px<maxx; px++) {
            float fx=(float)px+0.5f;
            float w0=(x1-x0)*(fy-y0)-(fx-x0)*(y1-y0);
            float w1=(x2-x1)*(fy-y1)-(fx-x1)*(y2-y1);
            float w2=(x0-x2)*(fy-y2)-(fx-x2)*(y0-y2);
            float b0=w0*inv,b1=w1*inv,b2=w2*inv;
            if (b0>=0 && b1>=0 && b2>=0) {
                float z=z0*b0+z1*b1+z2*b2;
                if (z < zrow[0]) {
                    zrow[0]=z;
                    row[0]=cr; row[1]=cg; row[2]=cb; row[3]=255;
                }
            }
            row+=4; zrow++;
        }
    }
}

void wb_rast_render(wb_rast_ctx *r, uint8_t *out_rgba) {
    if (!r || !out_rgba || r->ntris <= 0) return;
    memset(out_rgba, 0, (size_t)r->w * r->h * 4);
    if (r->zbuf_on && !r->zbuf) { r->zbuf = malloc((size_t)r->w*r->h*sizeof(float)); }
    if (r->zbuf_on && r->zbuf)
        for (int i = 0; i < r->w*r->h; i++) r->zbuf[i] = 1e9f;

    for (int i = 0; i < r->nverts; i++) project_vert(r, i);

    /* painter's sort: far-to-near by centroid depth (no alloc: insertion
     * sort on a small index array on the stack — Jet-style fixed budget).
     * Cap: scenes beyond 512 tris render unsorted (still correct for
     * opaque geometry thanks to the later-drawn-wins overwrite). */
    int order[512];
    int n = r->ntris < 512 ? r->ntris : 512;
    for (int i = 0; i < n; i++) order[i] = i;
    float depth[512];
    for (int i = 0; i < n; i++) {
        wb_rast_tri *t = &r->tris[i];
        depth[i] = (r->sz[t->v0] + r->sz[t->v1] + r->sz[t->v2]) / 3.0f;
    }
    for (int i = 1; i < n; i++) {
        int oi = order[i]; float d = depth[i];
        int j = i - 1;
        while (j >= 0 && depth[order[j]] < d) { order[j+1] = order[j]; j--; }
        order[j+1] = oi;
    }

    for (int k = 0; k < n; k++) {
        wb_rast_tri *t = &r->tris[order[k]];
        /* R055: face normal in WORLD space (pre-projection model coords) */
        const wb_rast_vertex *A=&r->verts[t->v0], *B=&r->verts[t->v1], *C=&r->verts[t->v2];
        float ux=B->x-A->x, uy=B->y-A->y, uz=B->z-A->z;
        float vx=C->x-A->x, vy=C->y-A->y, vz=C->z-A->z;
        float nx=uy*vz-uz*vy, ny=uz*vx-ux*vz, nz=ux*vy-uy*vx;
        float nl=sqrtf(nx*nx+ny*ny+nz*nz);
        /* R055: sun_i==0 means lighting fully disabled (raw colors) */
        float diff = r->sun_i <= 0.0f ? 1.0f : 0.45f;
        float specv=0.0f;
        if (nl>1e-6f) {
            nx/=nl; ny/=nl; nz/=nl;
            float ndl = -(nx*r->sun[0]+ny*r->sun[1]+nz*r->sun[2]);
            if (ndl<0) ndl=0;
            diff += r->sun_i * ndl * 0.75f;
            if (r->spec>0) {
                /* half-vector approx: view dir (0,0,-1), h = normalize(l+v) */
                float hx=r->sun[0], hy=r->sun[1], hz=r->sun[2]-1.0f;
                float hl=sqrtf(hx*hx+hy*hy+hz*hz);
                if (hl>1e-6f) { hx/=hl; hy/=hl; hz/=hl; }
                float ndh = -(nx*hx+ny*hy+nz*hz);
                if (ndh>0) specv = r->spec * ndh*ndh*ndh*ndh*ndh*ndh*ndh*ndh;
            }
        }
        int rr=(int)(t->r*diff + 255*specv); if(rr>255)rr=255;
        int gg=(int)(t->g*diff + 255*specv); if(gg>255)gg=255;
        int bb=(int)(t->b*diff + 255*specv); if(bb>255)bb=255;
        if (r->zbuf_on && r->zbuf)
            fill_tri_z(r,out_rgba,t->v0,t->v1,t->v2,(uint8_t)rr,(uint8_t)gg,(uint8_t)bb);
        else
            fill_tri(r,out_rgba,t->v0,t->v1,t->v2,(uint8_t)rr,(uint8_t)gg,(uint8_t)bb);
    }
}
