/* wbus/wbus_char2d.h — 2D character animation system API (R077).
 *
 * Bone rigging with FK + IK, skinned mesh deformation,
 * walk cycle, squash/stretch, and particle VFX.
 */

#ifndef WBUS_CHAR2D_H
#define WBUS_CHAR2D_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque character handle */
typedef struct wb_char2d wb_char2d;

/* ---- Lifecycle ---- */
wb_char2d *wb_char2d_create(void);
void       wb_char2d_destroy(wb_char2d *c);

/* ---- Bone Setup ---- */
int  wb_char2d_add_bone(wb_char2d *c, const char *name, int parent,
                          float length, float angle);
int  wb_char2d_add_vertex(wb_char2d *c, float x, float y,
                            int bone0, float weight);

/* ---- Transforms ---- */
void wb_char2d_set_position(wb_char2d *c, float x, float y);
void wb_char2d_set_scale(wb_char2d *c, float sx, float sy);
void wb_char2d_set_rotation(wb_char2d *c, float angle);

/* ---- Animation ---- */
void wb_char2d_update(wb_char2d *c, float dt);
void wb_char2d_ik_2bone(wb_char2d *c, int bone_a, int bone_b,
                          float target_x, float target_y);
void wb_char2d_squash_impulse(wb_char2d *c, float vx, float vy);

/* ---- Walk Cycle ---- */
typedef struct {
    float time, duration, stride_length, hip_bob;
} wb_walk_cycle;

void wb_char2d_walk_init(wb_walk_cycle *w);
void wb_char2d_walk_update(wb_char2d *c, wb_walk_cycle *w, float dt);

/* ---- Particle System ---- */
typedef struct wb_particles wb_particles;

wb_particles *wb_particles_create(void);
void          wb_particles_destroy(wb_particles *ps);
void          wb_particles_set_emitter(wb_particles *ps, float x, float y,
                                        float rate, float angle, float spread,
                                        float speed, float life, float gravity);
void          wb_particles_set_color(wb_particles *ps,
                                      uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void          wb_particles_set_additive(wb_particles *ps, int additive);
void          wb_particles_update(wb_particles *ps, float dt);
void          wb_particles_render(wb_particles *ps, uint8_t *rgba, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_CHAR2D_H */
