/* wbus_anim.h — keyframed 3D animation layer over wb_mesh/wb_rast (R054).
 *
 * The AGI-facing API: describe a scene as named OBJECTS with per-object
 * position/rotation/scale KEYFRAMES on a seconds timeline, then sample
 * frames and rasterize. This is what turns static meshes into "3D
 * diagrams and animations" for video export.
 *
 * Design: one anim = N objects; each object references a mesh TEMPLATE
 * (shared, not owned) plus its own channel tracks. Sampling is linear
 * interpolation between keys — cheap, predictable, good enough for
 * motion graphics.
 *
 * C11, opaque structs, self-contained.
 */
#ifndef WUBUS_WBUS_ANIM_H
#define WUBUS_WBUS_ANIM_H

#include <stdint.h>
#include <stddef.h>
#include "wbus_rast.h"
#include "wbus_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_anim wb_anim;

wb_anim *wb_anim_create(int width, int height);
void     wb_anim_free(wb_anim *a);

/* Add an object playing mesh `m` (NOT copied — must outlive the anim).
 * Returns object index or -1. */
int  wb_anim_add_object(wb_anim *a, const wb_mesh *m,
                        uint8_t r, uint8_t g, uint8_t b);

/* Keyframe an object's transform at time t (seconds). All channels set
 * together — simplest possible model, matches motion-graphics needs.
 * ease: 0=linear 1=ease-in-out 2=ease-out-bounce 3=elastic-out 4=ease-in */
int  wb_anim_key(wb_anim *a, int obj, double t,
                 float px, float py, float pz,
                 float rx, float ry, float rz,
                 float scale);
int  wb_anim_key_ease(wb_anim *a, int obj, double t,
                      float px, float py, float pz,
                      float rx, float ry, float rz,
                      float scale, int ease);

/* R055c: camera as a first-class animated object. Same keyframe API but
 * affecting the render camera (orbit angles + distance). */
int  wb_anim_set_camera(wb_anim *a, float rx, float ry, float dist);
int  wb_anim_key_camera(wb_anim *a, double t,
                        float rx, float ry, float dist);
/* per-frame camera override used during render (set by camera keys) */

/* R055c: parenting — child's transform composes after parent's. Objects
 * render in index order; parent must have a LOWER index than child. */
int  wb_anim_parent(wb_anim *a, int child, int parent);

/* Render frame at time t into out_rgba (w*h*4, alpha-keyed). Samples all
 * objects' channels at t, transforms each mesh's copy, renders once. */
void wb_anim_render_frame(wb_anim *a, double t, uint8_t *out_rgba);

/* Duration = latest key across all objects (0 if none). */
double wb_anim_duration(const wb_anim *a);

int  wb_anim_object_count(const wb_anim *a);


/* R074 hop 118 (G-SF003): per-channel keys. ease<0 keeps existing. */
int wb_anim_key_pos_x(wb_anim *a, int o, double t, float x, int ease);
int wb_anim_key_pos_y(wb_anim *a, int o, double t, float y, int ease);
int wb_anim_key_pos_z(wb_anim *a, int o, double t, float z, int ease);
int wb_anim_key_rot_z(wb_anim *a, int o, double t, float rz, int ease);
/* G-SF004: key editing. */
int wb_anim_key_delete(wb_anim *a, int obj, int key_idx);
/* G-SF056: visibility window. */
int wb_anim_set_visible(wb_anim *a, int obj, double from, double to);
/* G-SF006: cubic bezier flight path. */
int wb_anim_path_bezier(wb_anim *a, int obj, double t0, double t1,
                        float x0,float y0,float z0, float x1,float y1,float z1,
                        float x2,float y2,float z2, float x3,float y3,float z3,
                        int steps);
/* G-SF022: shortest-arc rotation unwrap across keys. */
int wb_anim_rot_unwrap(wb_anim *a, int obj);
/* G-SF010: billboard sprites. */
int wb_anim_set_billboard(wb_anim *a, int obj, int on);
/* G-SF009: yaw-to-camera. */
int wb_anim_set_lookcam(wb_anim *a, int obj, int on);
/* G-SF015: unlit/emissive object. */
int wb_anim_set_emissive(wb_anim *a, int obj, int on);
/* G-SF040: 4x supersampled anti-aliased frame. */
void wb_anim_render_frame_aa(wb_anim *a, double t, uint8_t *out_rgba);
/* G-SF036: temporal accumulation motion blur (caller-owned state). */
void wb_anim_render_frame_blur(wb_anim *a, double t, float blend,
                               uint8_t *state_rgba);
/* G-SF041: frame grab to PPM. */
int wb_anim_screenshot(wb_anim *a, double t, const char *path);
/* G-SF026: render-resolution override. */
int wb_anim_set_resolution(wb_anim *a, int w, int h);
int wb_anim_key_move(wb_anim *a, int obj, int key_idx, double new_t);

#ifdef __cplusplus
}
#endif


/* R074 hop 113/114: loop keys, fog, events, size query. */
void wb_anim_get_size(const wb_anim *a, int *w, int *h);
int wb_anim_key_loop(wb_anim *a, int obj, double dur,
                     double period, double phase,
                     float px, float py, float pz_far, float pz_near);
void wb_anim_set_fog(wb_anim *a, float z_near, float z_far,
                     uint8_t r, uint8_t g, uint8_t b);
int wb_anim_event_add(wb_anim *a, double t, int id);
int wb_anim_events_due(const wb_anim *a, double t_prev, double t_now,
                       int *out_ids, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_ANIM_H */

/* R074 hop 146: metadata & diagnostics */
#define WB_ANIM_OK 0
#define WB_ANIM_ERR_OBJS 1
#define WB_ANIM_ERR_KEYS 2
extern int wb_anim_last_error;
void wb_anim_set_rate(int fps);          /* G-SF054 */
int  wb_anim_get_rate(void);
const char *wb_anim_error_str(wb_anim *a);   /* G-SF089 */
void wb_anim_set_progress(void (*fn)(double frac, void *user), void *user); /* G-SF060 */
void wb_anim_progress(double frac);

/* G-SF008: camera shake composition (additive, deterministic). */
int wb_anim_set_shake(wb_anim *a, float amt);

/* G-SF019: instancing — draw src_obj again at a static transform. */
int wb_anim_add_instance(wb_anim *a, int src_obj,
                         float px, float py, float pz,
                         float rx, float ry, float rz, float s);

/* G-SF024: last frame's normalized depth (0 near .. 1 far). */
const float *wb_anim_depth_map(const wb_anim *a);

/* G-SF029: planar projected ground shadow at height y. */
int wb_anim_set_ground_shadow(wb_anim *a, float y);

/* G-SF092: the clock contract — engine samples -> anim seconds. */
double wb_anim_time_from_samples(double song_pos_samples);

/* G-SF025: distance-based LOD mesh substitution. */
int wb_anim_set_lod(wb_anim *a, int obj, const struct wb_mesh *m, float dist);

/* G-SF091: render a clip-bound scene letterboxed into a caller frame. */
void wb_anim_render_clip_scene(struct wb_anim *a, double t,
                               uint8_t *out, int out_w, int out_h);
