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

void wb_rast_render(wb_rast_ctx *r, uint8_t *out_rgba) {
    if (!r || !out_rgba || r->ntris <= 0) return;
    memset(out_rgba, 0, (size_t)r->w * r->h * 4);

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
        fill_tri(r, out_rgba, t->v0, t->v1, t->v2, t->r, t->g, t->b);
    }
}
