/* wb_cgi.c — self-contained low-poly 3D-CGI scene model (R043-G7).
 * Software pipeline: model rotate -> perspective project -> screen space.
 * Pure C11, stdlib only; no SDL/UI/render knowledge. See wbus_cgi.h. */

#include "wbus/wbus_cgi.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef WB_CGI_PI
#define WB_CGI_PI 3.14159265358979
#endif

struct wb_cgi_scene {
    float rx, ry, rz;          /* rotation (radians) */
    float zoom;                /* camera distance scale 0.25..4 */
    double t;                  /* accumulated time */

    /* cube mesh: 8 vertices, 12 triangle indices */
    float   verts[8][3];
    int     tris[12][3];
    /* projected output cache, recomputed on each accessor pass */
    float   px[8], py[8];      /* projected screen coords per vertex */
    float   depth[8];
    float   tri_shade[12];
    int     projected;
};

/* face shading by normal-ish hash — stable per-triangle so the cube reads
 * as a solid rotating body, not wireframe noise */
static float tri_shade_for(int i) {
    static const float shades[6] = { 0.95f, 0.75f, 0.60f, 0.85f, 0.50f, 0.68f };
    return shades[(i / 2) % 6];
}

wb_cgi_scene *wb_cgi_scene_create(void) {
    wb_cgi_scene *sc = calloc(1, sizeof(*sc));
    if (!sc) return NULL;
    sc->rx = 0.5f; sc->ry = 0.0f; sc->rz = 0.0f;
    sc->zoom = 1.0f;

    /* unit cube centered at origin */
    static const float cv[8][3] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}
    };
    memcpy(sc->verts, cv, sizeof(cv));
    /* 12 triangles (2 per face), outward winding */
    static const int ct[12][3] = {
        {0,1,2},{0,2,3},  /* back  z- */
        {4,6,5},{4,7,6},  /* front z+ */
        {0,4,5},{0,5,1},  /* bottom */
        {2,6,7},{2,7,3},  /* top */
        {0,3,7},{0,7,4},  /* left */
        {1,5,6},{1,6,2}   /* right */
    };
    memcpy(sc->tris, ct, sizeof(ct));
    for (int i = 0; i < 12; i++) sc->tri_shade[i] = tri_shade_for(i);
    sc->projected = 0;
    return sc;
}

void wb_cgi_scene_destroy(wb_cgi_scene *sc) { free(sc); }

void wb_cgi_scene_tick(wb_cgi_scene *sc, double dt) {
    if (!sc) return;
    sc->t += dt;
    sc->ry += (float)(dt * 0.7);           /* slow yaw spin */
    sc->rx = 0.45f + 0.15f * (float)sin(sc->t * 0.4);
    sc->projected = 0;
}

void wb_cgi_scene_set_rotation(wb_cgi_scene *sc, float rx, float ry, float rz) {
    if (!sc) return;
    sc->rx = rx; sc->ry = ry; sc->rz = rz;
    sc->projected = 0;
}
void wb_cgi_scene_get_rotation(const wb_cgi_scene *sc, float *rx, float *ry, float *rz) {
    if (!sc) return;
    if (rx) *rx = sc->rx;
    if (ry) *ry = sc->ry;
    if (rz) *rz = sc->rz;
}
void wb_cgi_scene_set_zoom(wb_cgi_scene *sc, float zoom) {
    if (!sc) return;
    if (zoom < 0.25f) zoom = 0.25f;
    if (zoom > 4.0f)  zoom = 4.0f;
    sc->zoom = zoom;
    sc->projected = 0;
}
float wb_cgi_scene_get_zoom(const wb_cgi_scene *sc) {
    return sc ? sc->zoom : 1.0f;
}

/* rotate + project all vertices once (lazy, invalidated by setters/tick) */
static void project(wb_cgi_scene *sc) {
    if (sc->projected) return;
    const float cxr = cosf(sc->rx), sxr = sinf(sc->rx);
    const float cyr = cosf(sc->ry), syr = sinf(sc->ry);
    const float czr = cosf(sc->rz), szr = sinf(sc->rz);
    const float cam_z = 6.0f / sc->zoom;      /* camera distance */
    const float focal = 300.0f * sc->zoom;    /* focal length in px */
    for (int i = 0; i < 8; i++) {
        float x = sc->verts[i][0], y = sc->verts[i][1], z = sc->verts[i][2];
        /* Rx */
        float y1 = y*cxr - z*sxr, z1 = y*sxr + z*cxr;
        /* Ry */
        float x2 = x*cyr + z1*syr, z2 = -x*syr + z1*cyr;
        /* Rz */
        float x3 = x2*czr - y1*szr, y3 = x2*szr + y1*czr;
        /* translate away from camera and perspective-divide */
        float zd = z2 + cam_z;
        if (zd < 0.1f) zd = 0.1f;
        sc->depth[i] = zd;
        sc->px[i] = x3 * focal / zd;
        sc->py[i] = y3 * focal / zd;
    }
    sc->projected = 1;
}

int wb_cgi_scene_tri_count(const wb_cgi_scene *sc) { return sc ? 12 : 0; }

void wb_cgi_scene_tri(const wb_cgi_scene *sc, int i,
                      float *x0, float *y0, float *x1, float *y1,
                      float *x2, float *y2, float *shade) {
    if (!sc || i < 0 || i >= 12) {
        if (x0)*x0=0; if (y0)*y0=0; if (x1)*x1=0; if (y1)*y1=0;
        if (x2)*x2=0; if (y2)*y2=0; if (shade)*shade=0;
        return;
    }
    /* const-cast: projection is a lazy cache, not a logical mutation */
    project((wb_cgi_scene *)sc);
    int a = sc->tris[i][0], b = sc->tris[i][1], c = sc->tris[i][2];
    if (x0)*x0 = sc->px[a]; if (y0)*y0 = sc->py[a];
    if (x1)*x1 = sc->px[b]; if (y1)*y1 = sc->py[b];
    if (x2)*x2 = sc->px[c]; if (y2)*y2 = sc->py[c];
    if (shade)*shade = sc->tri_shade[i];
}

int wb_cgi_scene_grid_count(const wb_cgi_scene *sc) { return sc ? 10 : 0; }

void wb_cgi_scene_grid_line(const wb_cgi_scene *sc, int i,
                            float *x0, float *y0, float *x1, float *y1) {
    if (!sc || i < 0 || i >= 10) {
        if (x0)*x0=0; if (y0)*y0=0; if (x1)*x1=0; if (y1)*y1=0;
        return;
    }
    /* ground grid at y=-2, lines parallel to X or Z, projected like verts */
    float gx0 = (i % 2 == 0) ? -4.0f : (float)(i/2) - 2.0f;
    float gz0 = (i % 2 == 0) ? (float)(i/2) - 2.0f : -4.0f;
    float gx1 = (i % 2 == 0) ?  4.0f : (float)(i/2) - 2.0f;
    float gz1 = (i % 2 == 0) ? (float)(i/2) - 2.0f :  4.0f;
    const wb_cgi_scene *m = sc;
    const float cyr = cosf(m->ry), syr = sinf(m->ry);
    const float cam_z = 6.0f / m->zoom;
    const float focal = 300.0f * m->zoom;
    float pts[2][2];
    float in[2][3] = { {gx0,-2,gz0}, {gx1,-2,gz1} };
    for (int k = 0; k < 2; k++) {
        float x = in[k][0], y = in[k][1], z = in[k][2];
        float x2 = x*cyr + z*syr, z2 = -x*syr + z*cyr;
        float zd = z2 + cam_z;
        if (zd < 0.1f) zd = 0.1f;
        pts[k][0] = x2 * focal / zd;
        pts[k][1] = y  * focal / zd;
    }
    if (x0)*x0 = pts[0][0]; if (y0)*y0 = pts[0][1];
    if (x1)*x1 = pts[1][0]; if (y1)*y1 = pts[1][1];
}
