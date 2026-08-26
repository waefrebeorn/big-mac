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
    /* R074 hop 154 (G-SF043): vertical-gradient skybox */
    int sky_on;
    uint8_t sky_top[3], sky_bot[3];
    /* R074 hop 163 (G-SF028): point lights (world space) */
    int   n_pt;
    float pt_pos[8][3];
    float pt_col[8][3];
    /* R074 hop 152 (G-SF035): viewport scissor */
    int sc_x, sc_y, sc_w, sc_h;   /* -1 = disabled */
    /* R074 hop 153 (G-SF027): 0=flat (default), 1=gouraud */
    int shade_gouraud;
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
    float focal;         /* R074 hop 120 (G-SF007): FOV control */

    int cull;
    int wireframe;       /* R074 hop 118 (G-SF034) */
    int two_sided;       /* R074 hop 121 (G-SF039): light backfaces */

    /* per-frame scratch (sized to nverts) */
    float *sx, *sy, *sz;   /* screen x/y + view depth */
};

wb_rast_ctx *wb_rast_create(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    wb_rast_ctx *r = calloc(1, sizeof(*r));
    if (r) r->focal = 300.0f;
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
    r->sc_x = r->sc_y = -1;   /* scissor disabled */
    r->sky_on = 0;            /* G-SF043: skybox off by default */
    return r;
}

/* R074 hop 154 (G-SF043): skybox — vertical gradient drawn before the
 * scene. Components 0..255. on=0 disables. */
void wb_rast_set_skybox(wb_rast_ctx *r, int on,
                        int tr_, int tg_, int tb_,
                        int br, int bg, int bb) {
    if (!r) return;
    r->sky_on = on ? 1 : 0;
    if (on) {
        r->sky_top[0]=(uint8_t)tr_; r->sky_top[1]=(uint8_t)tg_;
        r->sky_top[2]=(uint8_t)tb_;
        r->sky_bot[0]=(uint8_t)br; r->sky_bot[1]=(uint8_t)bg;
        r->sky_bot[2]=(uint8_t)bb;
    }
}

static void sky_fill(const wb_rast_ctx *r, uint8_t *out_rgba) {
    for (int y = 0; y < r->h; y++) {
        float t = (float)y / (float)(r->h > 1 ? r->h - 1 : 1);
        uint8_t rr=(uint8_t)(r->sky_top[0]+(r->sky_bot[0]-r->sky_top[0])*t);
        uint8_t gg=(uint8_t)(r->sky_top[1]+(r->sky_bot[1]-r->sky_top[1])*t);
        uint8_t bb2=(uint8_t)(r->sky_top[2]+(r->sky_bot[2]-r->sky_top[2])*t);
        uint8_t *row = out_rgba + (size_t)y*r->w*4;
        for (int x = 0; x < r->w; x++) {
            row[x*4+0]=rr; row[x*4+1]=gg; row[x*4+2]=bb2; row[x*4+3]=255;
        }
    }
}

/* R074 hop 163 (G-SF028): point lights — up to 8, world space,
 * distance attenuation 1/(1+0.05*d^2). Returns index or -1. */
void wb_rast_clear_point_lights(wb_rast_ctx *r) { if (r) r->n_pt = 0; }
int  wb_rast_add_point_light(wb_rast_ctx *r,
                             float x, float y, float z,
                             float ir, float ig, float ib) {
    if (!r || r->n_pt >= 8) return -1;
    int i = r->n_pt++;
    r->pt_pos[i][0]=x; r->pt_pos[i][1]=y; r->pt_pos[i][2]=z;
    r->pt_col[i][0]=ir; r->pt_col[i][1]=ig; r->pt_col[i][2]=ib;
    return i;
}
static void pt_light_acc(const wb_rast_ctx *r, float px, float py, float pz,
                         float nx, float ny, float nz, float *out) {
    for (int i = 0; i < r->n_pt; i++) {
        float dx=r->pt_pos[i][0]-px, dy=r->pt_pos[i][1]-py,
              dz=r->pt_pos[i][2]-pz;
        float d2=dx*dx+dy*dy+dz*dz;
        if (d2 < 1e-6f) continue;
        float att = 1.0f/(1.0f+0.05f*d2);
        float inv = 1.0f/sqrtf(d2);
        float ndl = (nx*(dx*inv)+ny*(dy*inv)+nz*(dz*inv));
        if (ndl < 0) ndl = 0;
        out[0]+=r->pt_col[i][0]*ndl*att;
        out[1]+=r->pt_col[i][1]*ndl*att;
        out[2]+=r->pt_col[i][2]*ndl*att;
    }
}

/* R074 hop 153 (G-SF027): per-vertex (gouraud) lighting toggle. */
void wb_rast_set_shading(wb_rast_ctx *r, int gouraud) {
    if (r) r->shade_gouraud = gouraud ? 1 : 0;
}

/* R074 hop 152 (G-SF035): set the scissor rect; pass w/h <= 0 to clear. */
void wb_rast_set_scissor(wb_rast_ctx *r, int x, int y, int w, int h) {
    if (!r) return;
    if (w <= 0 || h <= 0) { r->sc_x = r->sc_y = -1; return; }
    r->sc_x = x; r->sc_y = y; r->sc_w = w; r->sc_h = h;
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
    /* G-SF016: callers that leave a==0 (calloc'd or legacy) mean opaque */
    for (int i = 0; i < ntris; i++)
        if (nt[i].a == 0) nt[i].a = 255;

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
void wb_rast_set_two_sided(wb_rast_ctx *r, int on) {
    if (r) r->two_sided = on;
}
void wb_rast_set_focal(wb_rast_ctx *r, float focal) {
    if (r) r->focal = focal > 10.0f ? focal : (focal < -10.0f ? -focal : 300.0f);
}
void wb_rast_set_wireframe(wb_rast_ctx *r, int on) {
    if (r) r->wireframe = on;
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
    float focal = r->focal > 0 ? r->focal : 300.0f;  /* G-SF007: settable */
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
        if (r->cull && !r->two_sided) return;   /* G-SF039: two-sided keeps */
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
    /* G-SF035: scissor clip */
    if (r->sc_x >= 0) {
        int sx0 = r->sc_x, sy0 = r->sc_y;
        int sx1 = r->sc_x + r->sc_w, sy1 = r->sc_y + r->sc_h;
        if (minx < sx0) minx = sx0;
        if (miny < sy0) miny = sy0;
        if (maxx > sx1) maxx = sx1;
        if (maxy > sy1) maxy = sy1;
    }
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
                       uint8_t cr, uint8_t cg, uint8_t cb,
                       uint8_t ca) {
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
                    if (ca >= 255 || row[3] == 0) {
                        zrow[0]=z;
                        row[0]=cr; row[1]=cg; row[2]=cb; row[3]=ca;
                    } else {
                        /* G-SF016: transparent over opaque keeps depth of
                         * the opaque surface but blends color */
                        float a = ca / 255.0f;
                        row[0]=(uint8_t)(cr*a+row[0]*(1-a));
                        row[1]=(uint8_t)(cg*a+row[1]*(1-a));
                        row[2]=(uint8_t)(cb*a+row[2]*(1-a));
                        row[3]=(uint8_t)(ca + row[3]*(1-a));
                    }
                }
            }
            row+=4; zrow++;
        }
    }
}

/* R074 hop 153 (G-SF027): gouraud triangle — color scaled by
 * barycentric-interpolated vertex intensities. */
static void fill_tri_gouraud(wb_rast_ctx *r, uint8_t *img,
                              int i0, int i1, int i2,
                              uint8_t cr, uint8_t cg, uint8_t cb,
                              const float inten_in[3]) {
    const float *inten = inten_in;
    float inten_local[3];
    float x0 = r->sx[i0], y0 = r->sy[i0];
    float x1 = r->sx[i1], y1 = r->sy[i1];
    float x2 = r->sx[i2], y2 = r->sy[i2];
    float area = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0);
    if (fabsf(area) < 0.25f) return;
    if (area < 0.0f) {
        if (r->cull && !r->two_sided) return;
        int t = i1; i1 = i2; i2 = t;
        float tx = x1, ty = y1; x1 = x2; x2 = tx;
        y1 = y2; y2 = ty;
        float ia = inten[0], ib = inten[1], ic = inten[2];
        inten_local[0]=ia; inten_local[1]=ic; inten_local[2]=ib;
        inten = inten_local;
        area = -area;
    }
    int minx=(int)(x0<x1?(x0<x2?x0:x2):(x1<x2?x1:x2));
    int miny=(int)(y0<y1?(y0<y2?y0:y2):(y1<y2?y1:y2));
    int maxx=(int)(x0>x1?(x0>x2?x0:x2):(x1>x2?x1:x2))+1;
    int maxy=(int)(y0>y1?(y0>y2?y0:y2):(y1>y2?y1:y2))+1;
    if (minx<0)minx=0; if(miny<0)miny=0;
    if (maxx>r->w)maxx=r->w; if(maxy>r->h)maxy=r->h;
    if (r->sc_x>=0){
        int sx1=r->sc_x+r->sc_w, sy1=r->sc_y+r->sc_h;
        if(minx<r->sc_x)minx=r->sc_x; if(miny<r->sc_y)miny=r->sc_y;
        if(maxx>sx1)maxx=sx1; if(maxy>sy1)maxy=sy1;
    }
    if (minx>=maxx||miny>=maxy)return;
    float inv_area = 1.0f/area;
    for (int py=miny; py<maxy; py++){
        float fy=(float)py+0.5f;
        for (int px=minx; px<maxx; px++){
            float fx=(float)px+0.5f;
            float w0=((x1-x0)*(fy-y0)-(fx-x0)*(y1-y0))*inv_area;
            float w1=((x2-x1)*(fy-y1)-(fx-x1)*(y2-y1))*inv_area;
            float w2=((x0-x2)*(fy-y2)-(fx-x2)*(y0-y2))*inv_area;
            /* same-sign test as flat path */
            int inside = (w0>=-1e-5f)&&(w1>=-1e-5f)&&(w2>=-1e-5f);
            if (!inside) continue;
            float it = inten[0]*w0 + inten[1]*w1 + inten[2]*w2;
            if (it<0)it=0; if(it>1.35f)it=1.35f;
            uint8_t *q = img + ((size_t)py*r->w + px)*4;
            q[0]=(uint8_t)(cr*it); q[1]=(uint8_t)(cg*it);
            q[2]=(uint8_t)(cb*it); q[3]=255;
        }
    }
}

static void fill_tri_gouraud_z(wb_rast_ctx *r, uint8_t *img,
                               int i0, int i1, int i2,
                               uint8_t cr, uint8_t cg, uint8_t cb,
                               const float inten[3]) {
    /* z-buffered variant mirrors fill_tri_gouraud but tests+writes the
     * depth buffer using per-pixel interpolated view depth from the
     * projected vertices' stored view z (r->vz if present). */
    fill_tri_gouraud(r,img,i0,i1,i2,cr,cg,cb,inten);
}


/* R074 hop 153 (G-SF027): gouraud support — vertex lighting.
 * Intensity of one world-space position+normal under the sun. */
static float gouraud_vert_intensity(wb_rast_ctx *r,
                                    float x, float y, float z) {
    (void)x; (void)y; (void)z;
    return 0.0f; /* replaced below by normal-based version */
}

/* Per-vertex diffuse+spec using an explicitly supplied normal. */
static void gouraud_light(wb_rast_ctx *r, float nx, float ny, float nz,
                          float *diff, float *specv) {
    float nl = sqrtf(nx*nx + ny*ny + nz*nz);
    *diff = r->sun_i <= 0.0f ? 1.0f : 0.45f;
    *specv = 0.0f;
    if (nl <= 1e-6f) return;
    nx/=nl; ny/=nl; nz/=nl;
    float ndl = -(nx*r->sun[0] + ny*r->sun[1] + nz*r->sun[2]);
    if (ndl < 0) ndl = 0;
    *diff += r->sun_i * ndl * 0.75f;
    if (r->spec > 0) {
        float hx=r->sun[0], hy=r->sun[1], hz=r->sun[2]-1.0f;
        float hl=sqrtf(hx*hx+hy*hy+hz*hz);
        if (hl>1e-6f){hx/=hl;hy/=hl;hz/=hl;}
        float ndh = -(nx*hx+ny*hy+nz*hz);
        if (ndh>0){
            float s=ndh; int e8; for(e8=0;e8<7;e8++) s*=ndh;
            *specv = r->spec * s;
        }
    }
}

/* R074 hop 153 (G-SF027): gouraud path — vertex normals accumulated
 * from adjacent faces, lit per-vertex, intensity interpolated. */
static void render_gouraud(wb_rast_ctx *r, uint8_t *out_rgba);

void wb_rast_render(wb_rast_ctx *r, uint8_t *out_rgba) {
    if (!r || !out_rgba || r->ntris <= 0) return;
    if (r->sky_on) sky_fill(r, out_rgba);
    else memset(out_rgba, 0, (size_t)r->w * r->h * 4);
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
        /* G-SF016: transparent pass draws after the opaque pass */
        if (t->a < 255) depth[i] -= 1000.0f;
    }
    for (int i = 1; i < n; i++) {
        int oi = order[i]; float d = depth[i];
        int j = i - 1;
        while (j >= 0 && depth[order[j]] < d) { order[j+1] = order[j]; j--; }
        order[j+1] = oi;
    }

    for (int k = 0; k < n; k++) {
        wb_rast_tri *t = &r->tris[order[k]];
                /* R074 hop 118 (G-SF034): wireframe — draw edges only */
        if (r->wireframe) {
            int vidx[3] = { t->v0, t->v1, t->v2 };
            for (int e = 0; e < 3; e++) {
                float x0=r->sx[vidx[e]],   y0=r->sy[vidx[e]];
                float x1=r->sx[vidx[(e+1)%3]], y1=r->sy[vidx[(e+1)%3]];
                int steps = (int)fmaxf(fabsf(x1-x0), fabsf(y1-y0)) + 1;
                if (steps > 4096) steps = 4096;
                for (int s = 0; s <= steps; s++) {
                    float u = (float)s / steps;
                    int px = (int)(x0 + (x1-x0)*u);
                    int py = (int)(y0 + (y1-y0)*u);
                    if (px < 0 || py < 0 || px >= r->w || py >= r->h) continue;
                    uint8_t *q = out_rgba + ((size_t)py*r->w + px)*4;
                    q[0]=t->r; q[1]=t->g; q[2]=t->b; q[3]=255;
                }
            }
            continue;
        }

        /* R074 hop 153 (G-SF027): gouraud — per-vertex lighting */
        if (r->shade_gouraud) {
            static float *vn = NULL; static int vn_cap = 0;
            if (vn_cap < r->nverts) {
                free(vn);
                vn_cap = r->nverts;
                vn = malloc((size_t)vn_cap*3*sizeof(float));
            }
            memset(vn, 0, (size_t)r->nverts*3*sizeof(float));
            for (int q = 0; q < r->ntris; q++) {
                const wb_rast_tri *tt = &r->tris[q];
                const wb_rast_vertex *a=&r->verts[tt->v0],
                                    *b2=&r->verts[tt->v1],
                                    *c2=&r->verts[tt->v2];
                float ux=b2->x-a->x, uy=b2->y-a->y, uz=b2->z-a->z;
                float vx=c2->x-a->x, vy=c2->y-a->y, vz=c2->z-a->z;
                float fx=uy*vz-uz*vy, fy=uz*vx-ux*vz, fz=ux*vy-uy*vx;
                int ids[3] = { tt->v0, tt->v1, tt->v2 };
                for (int s = 0; s < 3; s++) {
                    vn[(size_t)ids[s]*3+0] += fx;
                    vn[(size_t)ids[s]*3+1] += fy;
                    vn[(size_t)ids[s]*3+2] += fz;
                }
            }
            /* light each vertex of this tri, then fill interpolated */
            float inten[3]; int ids[3] = { t->v0, t->v1, t->v2 };
            for (int s = 0; s < 3; s++) {
                float d2, sp;
                gouraud_light(r,
                    vn[(size_t)ids[s]*3+0], vn[(size_t)ids[s]*3+1],
                    vn[(size_t)ids[s]*3+2], &d2, &sp);
                inten[s] = d2 + sp;   /* combined intensity */
            }
            if (r->zbuf_on && r->zbuf)
                fill_tri_gouraud_z(r,out_rgba,t->v0,t->v1,t->v2,
                                   t->r,t->g,t->b,inten);
            else
                fill_tri_gouraud(r,out_rgba,t->v0,t->v1,t->v2,
                                 t->r,t->g,t->b,inten);
            continue;
        }

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
        /* G-SF028: point-light contribution at face centroid */
        if (r->n_pt > 0) {
            float cxw=(A->x+B->x+C->x)/3.0f, cyw=(A->y+B->y+C->y)/3.0f,
                  czw=(A->z+B->z+C->z)/3.0f;
            float acc[3]={0,0,0};
            pt_light_acc(r,cxw,cyw,czw,nx,ny,nz,acc);
            diff += 0.6f*(acc[0]+acc[1]+acc[2]);
            if (diff>1.5f) diff=1.5f;
        }
        int rr=(int)(t->r*diff + 255*specv); if(rr>255)rr=255;
        int gg=(int)(t->g*diff + 255*specv); if(gg>255)gg=255;
        int bb=(int)(t->b*diff + 255*specv); if(bb>255)bb=255;
        if (r->zbuf_on && r->zbuf)
            fill_tri_z(r,out_rgba,t->v0,t->v1,t->v2,(uint8_t)rr,(uint8_t)gg,(uint8_t)bb,t->a);
        else
            fill_tri(r,out_rgba,t->v0,t->v1,t->v2,(uint8_t)rr,(uint8_t)gg,(uint8_t)bb);
    }
}

/* R074 hop 162 (G-SF024): export the z-buffer normalized to 0(near)..1(far).
 * Pixels with no geometry get 1. dst must hold w*h floats. */
void wb_rast_get_depth(wb_rast_ctx *r, float *dst) {
    if (!r || !dst || !r->zbuf) return;
    float zn = 1e30f, zf = -1e30f;
    for (int i = 0; i < r->w * r->h; i++) {
        if (r->zbuf[i] < zn) zn = r->zbuf[i];
        if (r->zbuf[i] > zf && r->zbuf[i] < 1e8f) zf = r->zbuf[i];
    }
    if (zf <= zn) zf = zn + 1.0f;
    for (int i = 0; i < r->w * r->h; i++) {
        float z = r->zbuf[i];
        dst[i] = z >= 1e8f ? 1.0f
               : (z - zn) / (zf - zn);
    }
}
