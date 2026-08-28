/* wb_char2d.c — 2D character animation system (R077 Phase 1).
 *
 * Bone rigging with forward kinematics + inverse kinematics,
 * sprite mesh deformation, walk cycle, squash/stretch, and
 * a Verlet particle system for VFX.
 *
 * Built on wb_rast.c for rendering and wb_compositor.c for compositing.
 * Pure C11, no third party.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ===================================================================
 * 2D Bone System
 * =================================================================== */

#define WB_CHAR2D_MAX_BONES 64
#define WB_CHAR2D_MAX_VERTS 256
#define WB_CHAR2D_MAX_PARTICLES 512

/* A bone in rest pose: position relative to parent, length, angle */
typedef struct {
    float length;        /* bone length in pixels */
    float angle;         /* rest angle relative to parent (radians) */
    float rest_angle;    /* snapshot of angle at rest */
    int   parent;        /* parent bone index, -1 = root */
    char  name[32];
} wb_bone_def;

/* A bone in world space (computed each frame via FK) */
typedef struct {
    float x, y;          /* world position of bone start */
    float angle;         /* world angle */
    float cos_a, sin_a;  /* cached */
} wb_bone_world;

/* Vertex with skinning weights (up to 4 bone influences) */
typedef struct {
    float x, y;          /* rest position */
    int   bone[4];       /* bone indices */
    float w[4];          /* weights (sum to 1.0) */
    int   nbones;        /* 1-4 bone influences */
} wb_skinned_vertex;

/* ===================================================================
 * 2D Character
 * =================================================================== */

typedef struct {
    /* Bones */
    wb_bone_def    bones[WB_CHAR2D_MAX_BONES];
    wb_bone_world  world[WB_CHAR2D_MAX_BONES];
    int            nbones;

    /* Skinned mesh */
    wb_skinned_vertex verts[WB_CHAR2D_MAX_VERTS];
    int            nverts;

    /* Per-vertex deformed output (computed each frame) */
    float          deformed_x[WB_CHAR2D_MAX_VERTS];
    float          deformed_y[WB_CHAR2D_MAX_VERTS];

    /* Character transform */
    float pos_x, pos_y;
    float scale_x, scale_y;
    float rotation;

    /* Squash/stretch state */
    float squash_x, squash_y;  /* current deformation (1.0 = none) */
    float squash_vel_x, squash_vel_y;  /* velocity for spring */
} wb_char2d;

/* ===================================================================
 * Forward Kinematics
 * =================================================================== */

static void wb_char2d_fk(wb_char2d *c) {
    for (int i = 0; i < c->nbones; i++) {
        wb_bone_def *def = &c->bones[i];
        wb_bone_world *w = &c->world[i];

        if (def->parent < 0) {
            /* Root bone: position = character position */
            w->x = c->pos_x;
            w->y = c->pos_y;
            w->angle = c->rotation + def->angle;
        } else {
            /* Child bone: position = parent end, angle = parent angle + relative */
            wb_bone_world *pw = &c->world[def->parent];
            w->x = pw->x + pw->cos_a * c->bones[def->parent].length;
            w->y = pw->y + pw->sin_a * c->bones[def->parent].length;
            w->angle = pw->angle + def->angle;
        }
        w->cos_a = cosf(w->angle);
        w->sin_a = sinf(w->angle);
    }
}

/* ===================================================================
 * Inverse Kinematics (2-bone analytical solution)
 * =================================================================== */

/* Solve IK for a 2-bone chain (bone_a -> bone_b) to reach target (tx, ty).
 * bone_a is the parent (e.g. upper arm), bone_b is the child (e.g. forearm).
 * Returns 0 on success, -1 if target unreachable.
 */
static int wb_char2d_ik_2bone(wb_char2d *c, int bone_a, int bone_b,
                                float tx, float ty) {
    if (bone_a < 0 || bone_b < 0) return -1;
    if (c->bones[bone_b].parent != bone_a) return -1;  /* must be direct child */

    float len_a = c->bones[bone_a].length;
    float len_b = c->bones[bone_b].length;

    /* Position of bone_a start (the shoulder joint) */
    float ax, ay;
    int pa = c->bones[bone_a].parent;
    if (pa < 0) {
        ax = c->pos_x;
        ay = c->pos_y;
    } else {
        ax = c->world[pa].x + c->world[pa].cos_a * c->bones[pa].length;
        ay = c->world[pa].y + c->world[pa].sin_a * c->bones[pa].length;
    }

    float dx = tx - ax;
    float dy = ty - ay;
    float dist = sqrtf(dx*dx + dy*dy);

    /* Clamp to reachable range */
    float max_reach = len_a + len_b - 1.0f;
    float min_reach = fabsf(len_a - len_b) + 1.0f;
    if (dist > max_reach) dist = max_reach;
    if (dist < min_reach) dist = min_reach;

    /* Angle to target from shoulder */
    float target_angle = atan2f(dy, dx);

    /* Law of cosines: angle between bone_a and target line */
    float cos_angle_a = (len_a*len_a + dist*dist - len_b*len_b) / (2.0f*len_a*dist);
    if (cos_angle_a > 1.0f) cos_angle_a = 1.0f;
    if (cos_angle_a < -1.0f) cos_angle_a = -1.0f;
    float angle_offset = acosf(cos_angle_a);

    /* Set bone_a angle: absolute angle = target ± offset */
    float bone_a_abs = target_angle - angle_offset;

    /* Convert to relative angle (subtract parent world angle) */
    float parent_angle = (pa < 0) ? c->rotation : c->world[pa].angle;
    c->bones[bone_a].angle = bone_a_abs - parent_angle;

    /* Law of cosines: inner angle at elbow */
    float cos_angle_b = (len_a*len_a + len_b*len_b - dist*dist) / (2.0f*len_a*len_b);
    if (cos_angle_b > 1.0f) cos_angle_b = 1.0f;
    if (cos_angle_b < -1.0f) cos_angle_b = -1.0f;
    float inner_b = acosf(cos_angle_b);

    /* bone_b relative angle = π - inner_b (straight = 0 relative) */
    c->bones[bone_b].angle = M_PI - inner_b;

    /* Re-run FK to update world positions */
    wb_char2d_fk(c);
    return 0;
}

/* ===================================================================
 * Skinning: deform mesh vertices based on bone transforms
 * =================================================================== */

static void wb_char2d_skin(wb_char2d *c) {
    for (int v = 0; v < c->nverts; v++) {
        wb_skinned_vertex *sv = &c->verts[v];
        float out_x = 0, out_y = 0;

        for (int b = 0; b < sv->nbones; b++) {
            int bi = sv->bone[b];
            if (bi < 0 || bi >= c->nbones) continue;
            float w = sv->w[b];

            /* Transform vertex by bone's world transform */
            /* Vertex relative to bone start, rotated by bone angle */
            float bx = c->world[bi].x;
            float by = c->world[bi].y;
            float ca = c->world[bi].cos_a;
            float sa = c->world[bi].sin_a;

            /* Rest position relative to bone start */
            float rx = sv->x - (bi == 0 ? c->pos_x : c->world[c->bones[bi].parent].x + c->world[c->bones[bi].parent].cos_a * c->bones[c->bones[bi].parent].length);
            float ry = sv->y - (bi == 0 ? c->pos_y : c->world[c->bones[bi].parent].y + c->world[c->bones[bi].parent].sin_a * c->bones[c->bones[bi].parent].length);

            /* Rotate by bone angle */
            float rot_x = ca * rx - sa * ry;
            float rot_y = sa * rx + ca * ry;

            out_x += w * (bx + rot_x);
            out_y += w * (by + rot_y);
        }

        c->deformed_x[v] = out_x;
        c->deformed_y[v] = out_y;
    }
}

/* ===================================================================
 * Squash & Stretch (spring-based)
 * =================================================================== */

static void wb_char2d_squash_update(wb_char2d *c, float dt) {
    /* Spring parameters */
    float stiffness = 80.0f;
    float damping = 8.0f;

    /* Spring force toward 1.0 (no deformation) */
    float force_x = -stiffness * (c->squash_x - 1.0f) - damping * c->squash_vel_x;
    float force_y = -stiffness * (c->squash_y - 1.0f) - damping * c->squash_vel_y;

    c->squash_vel_x += force_x * dt;
    c->squash_vel_y += force_y * dt;
    c->squash_x += c->squash_vel_x * dt;
    c->squash_y += c->squash_vel_y * dt;

    /* Clamp to reasonable range */
    if (c->squash_x < 0.3f) c->squash_x = 0.3f;
    if (c->squash_x > 2.0f) c->squash_x = 2.0f;
    if (c->squash_y < 0.3f) c->squash_y = 0.3f;
    if (c->squash_y > 2.0f) c->squash_y = 2.0f;
}

/* Trigger a squash impulse (e.g. on landing) */
static void wb_char2d_squash_impulse(wb_char2d *c, float vx, float vy) {
    /* Volume preservation: sx = 1/sy */
    float impact = sqrtf(vx*vx + vy*vy) * 0.01f;
    if (impact > 0.5f) impact = 0.5f;
    c->squash_vel_x -= impact;
    c->squash_vel_y += impact;  /* stretch vertically when squashing horizontally */
}

/* ===================================================================
 * Walk Cycle Animation
 * =================================================================== */

typedef struct {
    float time;
    float duration;      /* seconds per cycle */
    float stride_length; /* pixels per step */
    float hip_bob;       /* vertical hip oscillation amplitude */
    int   contact_frame; /* frame index for foot contact */
} wb_walk_cycle;

/* Update bone angles for a walk cycle. Standard 4-phase walk:
 * contact → down → pass → up (×2 for each leg)
 */
static void wb_char2d_walk_update(wb_char2d *c, wb_walk_cycle *w, float dt) {
    w->time += dt;
    if (w->time >= w->duration) w->time -= w->duration;

    float phase = w->time / w->duration;  /* 0..1 */

    /* Hip bob: sinusoidal vertical oscillation */
    float bob = sinf(phase * 2.0f * M_PI * 2.0f) * w->hip_bob;  /* 2 bobs per cycle */
    c->pos_y += bob;  /* offset from base position */

    /* Leg swing: each leg is a sine wave, 180° out of phase */
    /* Assuming bone layout: 0=spine, 1=hip_L, 2=knee_L, 3=hip_R, 4=knee_R */
    if (c->nbones >= 5) {
        float swing_L = sinf(phase * 2.0f * M_PI) * 0.6f;  /* ±0.6 rad swing */
        float swing_R = sinf(phase * 2.0f * M_PI + M_PI) * 0.6f;

        c->bones[1].angle = swing_L;       /* hip_L */
        c->bones[3].angle = swing_R;       /* hip_R */

        /* Knee bends on up phase (when leg is behind) */
        float knee_L = fmaxf(0, -sinf(phase * 2.0f * M_PI + M_PI*0.5f)) * 0.8f;
        float knee_R = fmaxf(0, -sinf(phase * 2.0f * M_PI + M_PI*1.5f)) * 0.8f;
        c->bones[2].angle = knee_L;
        c->bones[4].angle = knee_R;
    }

    /* Arm swing: opposite to legs */
    if (c->nbones >= 7) {
        float arm_L = sinf(phase * 2.0f * M_PI + M_PI) * 0.4f;
        float arm_R = sinf(phase * 2.0f * M_PI) * 0.4f;
        c->bones[5].angle = arm_L;
        c->bones[6].angle = arm_R;
    }

    /* Forward movement */
    c->pos_x += w->stride_length * dt / w->duration;
}

/* ===================================================================
 * Particle System (Verlet integration)
 * =================================================================== */

typedef struct {
    float x, y;          /* position */
    float px, py;        /* previous position (Verlet) */
    float vx, vy;        /* explicit velocity (for forces) */
    float life;          /* remaining life (seconds) */
    float max_life;
    float size;
    uint8_t r, g, b, a;
    int   active;
} wb_particle;

typedef struct {
    wb_particle particles[WB_CHAR2D_MAX_PARTICLES];
    int count;

    /* Emitter */
    float emit_x, emit_y;
    float emit_rate;     /* particles per second */
    float emit_accum;
    float spread;        /* emission angle spread (radians) */
    float emit_angle;    /* base emission direction */
    float emit_speed;    /* initial speed */
    float emit_speed_var;
    float particle_life;
    float gravity;
    uint8_t r, g, b, a;
    int   additive;      /* additive blend mode */
} wb_particles;

static void wb_particles_init(wb_particles *ps) {
    memset(ps, 0, sizeof(*ps));
    ps->emit_rate = 50.0f;
    ps->particle_life = 1.0f;
    ps->gravity = 200.0f;
    ps->emit_speed = 100.0f;
    ps->spread = M_PI * 0.5f;
    ps->emit_angle = -M_PI * 0.5f;  /* upward */
    ps->r = 255; ps->g = 200; ps->b = 50; ps->a = 255;
}

static void wb_particles_emit(wb_particles *ps, float dt) {
    ps->emit_accum += ps->emit_rate * dt;
    while (ps->emit_accum >= 1.0f) {
        ps->emit_accum -= 1.0f;

        /* Find inactive particle */
        int idx = -1;
        for (int i = 0; i < WB_CHAR2D_MAX_PARTICLES; i++) {
            if (!ps->particles[i].active) { idx = i; break; }
        }
        if (idx < 0) return;  /* all active */

        wb_particle *p = &ps->particles[idx];
        p->active = 1;
        p->x = ps->emit_x;
        p->y = ps->emit_y;
        p->px = p->x;
        p->py = p->y;

        float angle = ps->emit_angle + ((float)rand() / RAND_MAX - 0.5f) * ps->spread;
        float speed = ps->emit_speed + ((float)rand() / RAND_MAX - 0.5f) * ps->emit_speed_var;
        p->vx = cosf(angle) * speed;
        p->vy = sinf(angle) * speed;

        p->life = ps->particle_life * (0.8f + 0.4f * (float)rand() / RAND_MAX);
        p->max_life = p->life;
        p->size = 2.0f + 4.0f * (float)rand() / RAND_MAX;
        p->r = ps->r; p->g = ps->g; p->b = ps->b; p->a = ps->a;
    }
}

static void wb_particles_update(wb_particles *ps, float dt) {
    for (int i = 0; i < WB_CHAR2D_MAX_PARTICLES; i++) {
        wb_particle *p = &ps->particles[i];
        if (!p->active) continue;

        /* Verlet integration */
        float vx = (p->x - p->px);
        float vy = (p->y - p->py);

        p->px = p->x;
        p->py = p->y;

        /* Apply gravity */
        vy += ps->gravity * dt;

        p->x += vx;
        p->y += vy;

        p->life -= dt;
        if (p->life <= 0.0f) {
            p->active = 0;
        }
    }
}

/* Render particles to RGBA buffer (additive or alpha blend) */
static void wb_particles_render(wb_particles *ps, uint8_t *rgba, int w, int h) {
    for (int i = 0; i < WB_CHAR2D_MAX_PARTICLES; i++) {
        wb_particle *p = &ps->particles[i];
        if (!p->active) continue;

        int ix = (int)p->x;
        int iy = (int)p->y;
        if (ix < 0 || ix >= w || iy < 0 || iy >= h) continue;

        float alpha = (p->life / p->max_life) * (p->a / 255.0f);
        int idx = (iy * w + ix) * 4;

        if (ps->additive) {
            rgba[idx+0] = (uint8_t)fminf(255, rgba[idx+0] + p->r * alpha);
            rgba[idx+1] = (uint8_t)fminf(255, rgba[idx+1] + p->g * alpha);
            rgba[idx+2] = (uint8_t)fminf(255, rgba[idx+2] + p->b * alpha);
        } else {
            float inv_a = 1.0f - alpha;
            rgba[idx+0] = (uint8_t)(p->r * alpha + rgba[idx+0] * inv_a);
            rgba[idx+1] = (uint8_t)(p->g * alpha + rgba[idx+1] * inv_a);
            rgba[idx+2] = (uint8_t)(p->b * alpha + rgba[idx+2] * inv_a);
        }
        rgba[idx+3] = 255;
    }
}

/* ===================================================================
 * Character Creation Helpers
 * =================================================================== */

static wb_char2d *wb_char2d_create(void) {
    wb_char2d *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->scale_x = 1.0f;
    c->scale_y = 1.0f;
    c->squash_x = 1.0f;
    c->squash_y = 1.0f;
    return c;
}

static void wb_char2d_destroy(wb_char2d *c) {
    free(c);
}

/* Add a bone. Returns bone index. */
static int wb_char2d_add_bone(wb_char2d *c, const char *name, int parent,
                               float length, float angle) {
    if (c->nbones >= WB_CHAR2D_MAX_BONES) return -1;
    int i = c->nbones++;
    wb_bone_def *b = &c->bones[i];
    strncpy(b->name, name, 31);
    b->name[31] = '\0';
    b->parent = parent;
    b->length = length;
    b->angle = angle;
    b->rest_angle = angle;
    return i;
}

/* Add a skinned vertex */
static int wb_char2d_add_vertex(wb_char2d *c, float x, float y,
                                 int bone0, float w0) {
    if (c->nverts >= WB_CHAR2D_MAX_VERTS) return -1;
    int i = c->nverts++;
    wb_skinned_vertex *v = &c->verts[i];
    v->x = x;
    v->y = y;
    v->bone[0] = bone0;
    v->w[0] = w0;
    v->nbones = 1;
    return i;
}

/* Full update: FK + skinning + squash */
static void wb_char2d_update(wb_char2d *c, float dt) {
    wb_char2d_fk(c);
    wb_char2d_skin(c);
    wb_char2d_squash_update(c, dt);
}
