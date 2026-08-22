/*
 * wbus_cgi.h — self-contained low-poly 3D-CGI scene model (R043-G7).
 *
 * A minimal software 3D pipeline: a mesh (vertices + triangle indices),
 * rotation angles, and a perspective camera. The UI asks for the projected
 * 2D triangles each frame and rasterizes them with SDL_RenderDrawLine —
 * the module knows nothing about SDL, the DAW, or the UI. Opaque struct,
 * C11, stdlib only. This is the 3D-CGI workspace tier's engine.
 */
#ifndef WUBUS_WBUS_CGI_H
#define WUBUS_WBUS_CGI_H

typedef struct wb_cgi_scene wb_cgi_scene;

/* Create the demo scene: a rotating cube (8 verts / 12 tris) + ground grid. */
wb_cgi_scene *wb_cgi_scene_create(void);
void          wb_cgi_scene_destroy(wb_cgi_scene *sc);

/* Animate: advances rotation by dt seconds. */
void wb_cgi_scene_tick(wb_cgi_scene *sc, double dt);

/* Manual control (UI dials / AGI bridge write here). */
void wb_cgi_scene_set_rotation(wb_cgi_scene *sc, float rx, float ry, float rz);
void wb_cgi_scene_get_rotation(const wb_cgi_scene *sc, float *rx, float *ry, float *rz);
void wb_cgi_scene_set_zoom(wb_cgi_scene *sc, float zoom);   /* 0.25..4.0 */
float wb_cgi_scene_get_zoom(const wb_cgi_scene *sc);

/* Projected-triangle access for the UI rasterizer.
 * Returns the triangle count and fills (x,y) screen-space vertices for
 * triangle i (3 vertices, clockwise). Viewport is w x h pixels centered
 * at (w/2, h*0.6). All coordinates are finite floats. */
int  wb_cgi_scene_tri_count(const wb_cgi_scene *sc);
void wb_cgi_scene_tri(const wb_cgi_scene *sc, int i,
                      float *x0, float *y0, float *x1, float *y1,
                      float *x2, float *y2, float *shade);

/* Grid-line access for the ground plane (2 endpoints per line). */
int  wb_cgi_scene_grid_count(const wb_cgi_scene *sc);
void wb_cgi_scene_grid_line(const wb_cgi_scene *sc, int i,
                            float *x0, float *y0, float *x1, float *y1);

#endif /* WUBUS_WBUS_CGI_H */
