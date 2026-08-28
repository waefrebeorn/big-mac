/* test_char2d.c — verify 2D character animation system */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* Minimal types */
typedef struct { float x, y; } vec2;

/* ---- Inline the char2d system for standalone test ---- */
#define WB_CHAR2D_MAX_BONES 64
#define WB_CHAR2D_MAX_VERTS 256
#define WB_CHAR2D_MAX_PARTICLES 512

typedef struct {
    float length, angle, rest_angle;
    int parent;
    char name[32];
} wb_bone_def;

typedef struct {
    float x, y, angle, cos_a, sin_a;
} wb_bone_world;

typedef struct {
    float x, y;
    int bone[4]; float w[4]; int nbones;
} wb_skinned_vertex;

typedef struct {
    wb_bone_def bones[WB_CHAR2D_MAX_BONES];
    wb_bone_world world[WB_CHAR2D_MAX_BONES];
    int nbones;
    wb_skinned_vertex verts[WB_CHAR2D_MAX_VERTS];
    int nverts;
    float deformed_x[WB_CHAR2D_MAX_VERTS];
    float deformed_y[WB_CHAR2D_MAX_VERTS];
    float pos_x, pos_y, scale_x, scale_y, rotation;
    float squash_x, squash_y, squash_vel_x, squash_vel_y;
} wb_char2d;

static void wb_char2d_fk(wb_char2d *c) {
    for (int i = 0; i < c->nbones; i++) {
        wb_bone_def *def = &c->bones[i];
        wb_bone_world *w = &c->world[i];
        if (def->parent < 0) {
            w->x = c->pos_x; w->y = c->pos_y;
            w->angle = c->rotation + def->angle;
        } else {
            wb_bone_world *pw = &c->world[def->parent];
            w->x = pw->x + pw->cos_a * c->bones[def->parent].length;
            w->y = pw->y + pw->sin_a * c->bones[def->parent].length;
            w->angle = pw->angle + def->angle;
        }
        w->cos_a = cosf(w->angle);
        w->sin_a = sinf(w->angle);
    }
}

static int wb_char2d_ik_2bone(wb_char2d *c, int bone_a, int bone_b, float tx, float ty) {
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

    float dx = tx - ax, dy = ty - ay;
    float dist = sqrtf(dx*dx + dy*dy);
    float max_r = len_a + len_b - 1.0f, min_r = fabsf(len_a - len_b) + 1.0f;
    if (dist > max_r) dist = max_r;
    if (dist < min_r) dist = min_r;

    /* Angle to target from shoulder */
    float target_angle = atan2f(dy, dx);

    /* Law of cosines: angle between bone_a and target line */
    float cos_a = (len_a*len_a + dist*dist - len_b*len_b) / (2.0f*len_a*dist);
    if (cos_a > 1) cos_a = 1; if (cos_a < -1) cos_a = -1;
    float angle_offset = acosf(cos_a);

    /* Set bone_a angle: absolute angle = target ± offset */
    float bone_a_abs = target_angle - angle_offset;

    /* Convert to relative angle (subtract parent world angle) */
    float parent_angle = (pa < 0) ? c->rotation : c->world[pa].angle;
    c->bones[bone_a].angle = bone_a_abs - parent_angle;

    /* Law of cosines: inner angle at elbow */
    float cos_b = (len_a*len_a + len_b*len_b - dist*dist) / (2.0f*len_a*len_b);
    if (cos_b > 1) cos_b = 1; if (cos_b < -1) cos_b = -1;
    float inner_b = acosf(cos_b);

    /* bone_b relative to bone_a: straight = π, so bend = π - inner */
    c->bones[bone_b].angle = M_PI - inner_b;

    /* Re-run FK to update world positions */
    wb_char2d_fk(c);
    return 0;
}

static void wb_char2d_skin(wb_char2d *c) {
    for (int v = 0; v < c->nverts; v++) {
        wb_skinned_vertex *sv = &c->verts[v];
        float ox = 0, oy = 0;
        for (int b = 0; b < sv->nbones; b++) {
            int bi = sv->bone[b];
            if (bi < 0 || bi >= c->nbones) continue;
            float w = sv->w[b];
            float bx = c->world[bi].x, by = c->world[bi].y;
            float ca = c->world[bi].cos_a, sa = c->world[bi].sin_a;
            float rx = sv->x, ry = sv->y;
            float rot_x = ca * rx - sa * ry;
            float rot_y = sa * rx + ca * ry;
            ox += w * (bx + rot_x);
            oy += w * (by + rot_y);
        }
        c->deformed_x[v] = ox;
        c->deformed_y[v] = oy;
    }
}

static void wb_char2d_squash_update(wb_char2d *c, float dt) {
    float k = 80.0f, d = 8.0f;
    float fx = -k*(c->squash_x-1.0f) - d*c->squash_vel_x;
    float fy = -k*(c->squash_y-1.0f) - d*c->squash_vel_y;
    c->squash_vel_x += fx*dt; c->squash_vel_y += fy*dt;
    c->squash_x += c->squash_vel_x*dt;
    c->squash_y += c->squash_vel_y*dt;
}

static void wb_char2d_squash_impulse(wb_char2d *c, float vx, float vy) {
    float impact = sqrtf(vx*vx + vy*vy) * 0.01f;
    if (impact > 0.5f) impact = 0.5f;
    c->squash_vel_x -= impact;
    c->squash_vel_y += impact;
}

/* ---- Test ---- */
int main(void) {
    wb_char2d c;
    memset(&c, 0, sizeof(c));
    c.squash_x = 1.0f; c.squash_y = 1.0f;
    c.pos_x = 100.0f; c.pos_y = 100.0f;

    /* Build a simple humanoid:
     * 0: root (spine), 1: upper_arm_L, 2: lower_arm_L, 3: upper_leg_L, 4: lower_leg_L
     */
    int root = c.nbones++;
    c.bones[root] = (wb_bone_def){40, 0, 0, -1, "spine"};
    int ua = c.nbones++;
    c.bones[ua] = (wb_bone_def){30, -0.5f, -0.5f, root, "upper_arm_L"};
    int la = c.nbones++;
    c.bones[la] = (wb_bone_def){25, 0.3f, 0.3f, ua, "lower_arm_L"};
    int ul = c.nbones++;
    c.bones[ul] = (wb_bone_def){35, 1.2f, 1.2f, root, "upper_leg_L"};
    int ll = c.nbones++;
    c.bones[ll] = (wb_bone_def){30, -0.2f, -0.2f, ul, "lower_leg_L"};

    /* Add skinned vertices (simple: one per bone end) */
    c.nverts = 0;
    c.verts[0] = (wb_skinned_vertex){0, 0, {root}, {1.0f}, 1};
    c.verts[1] = (wb_skinned_vertex){40, 0, {root}, {1.0f}, 1};
    c.verts[2] = (wb_skinned_vertex){70, 0, {ua}, {1.0f}, 1};
    c.verts[3] = (wb_skinned_vertex){95, 0, {la}, {1.0f}, 1};
    c.verts[4] = (wb_skinned_vertex){40, 35, {ul}, {1.0f}, 1};
    c.verts[5] = (wb_skinned_vertex){40, 65, {ll}, {1.0f}, 1};
    c.nverts = 6;

    /* Test FK */
    wb_char2d_fk(&c);
    wb_char2d_skin(&c);
    printf("FK test: root=(%.1f,%.1f) arm_end=(%.1f,%.1f) leg_end=(%.1f,%.1f)\n",
           c.world[root].x, c.world[root].y,
           c.world[la].x, c.world[la].y,
           c.world[ll].x, c.world[ll].y);

    /* Test IK: move hand to target */
    float tx = 150.0f, ty = 80.0f;
    int ret = wb_char2d_ik_2bone(&c, ua, la, tx, ty);
    /* Hand position = end of bone_la = world pos + length * direction */
    float hand_x = c.world[la].x + c.world[la].cos_a * c.bones[la].length;
    float hand_y = c.world[la].y + c.world[la].sin_a * c.bones[la].length;
    float hand_dx = hand_x - tx;
    float hand_dy = hand_y - ty;
    float hand_err = sqrtf(hand_dx*hand_dx + hand_dy*hand_dy);
    printf("IK test: target=(%.0f,%.0f) hand=(%.1f,%.1f) err=%.2f ret=%d\n",
           tx, ty, hand_x, hand_y, hand_err, ret);

    /* Test squash/stretch */
    wb_char2d_squash_impulse(&c, 0, 500.0f);
    for (int i = 0; i < 60; i++) {
        wb_char2d_squash_update(&c, 1.0f/60.0f);
    }
    printf("Squash test: after impulse + 1 sec: sx=%.3f sy=%.3f (should be ~1.0)\n",
           c.squash_x, c.squash_y);

    /* Verify */
    int pass = 1;
    if (hand_err > 5.0f) { printf("FAIL: IK error too large\n"); pass = 0; }
    if (c.squash_x < 0.5f || c.squash_x > 1.5f) { printf("FAIL: squash didn't settle\n"); pass = 0; }
    if (ret != 0) { printf("FAIL: IK returned error\n"); pass = 0; }

    printf("%s\n", pass ? "ALL PASS" : "SOME FAIL");
    return pass ? 0 : 1;
}
