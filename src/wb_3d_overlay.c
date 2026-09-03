/* wb_3d_overlay.c — 3D Character Overlay System (R098).
 * 60+ meta-layer abilities for YTP/YTPMV production.
 * Uses wb_omesh (lightweight overlay mesh) separate from wb_mesh (opaque CGI mesh).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wbus/wbus_compositor.h"
#include "wbus/wbus_vfx.h"

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* Math utilities (types from header) */
static inline float wb_clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float wb_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline float wb_vec3_dot(wb_o_vec3 a, wb_o_vec3 b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

static inline float wb_vec3_len(wb_o_vec3 v) {
    return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
}

static inline wb_o_vec3 wb_vec3_norm(wb_o_vec3 v) {
    float l = wb_vec3_len(v);
    if (l < 1e-6f) return (wb_o_vec3){0,0,0};
    return (wb_o_vec3){v.x/l, v.y/l, v.z/l};
}

static inline wb_o_vec3 wb_vec3_cross(wb_o_vec3 a, wb_o_vec3 b) {
    return (wb_o_vec3){
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x
    };
}

static inline wb_o_vec3 wb_vec3_sub(wb_o_vec3 a, wb_o_vec3 b) {
    return (wb_o_vec3){a.x-b.x, a.y-b.y, a.z-b.z};
}

static inline wb_o_vec3 wb_vec3_add(wb_o_vec3 a, wb_o_vec3 b) {
    return (wb_o_vec3){a.x+b.x, a.y+b.y, a.z+b.z};
}

static inline wb_o_vec3 wb_vec3_scale(wb_o_vec3 v, float s) {
    return (wb_o_vec3){v.x*s, v.y*s, v.z*s};
}

/* 4x4 matrix: column-major */
/* (non-static for test access) */
void wb_mat4_identity(wb_o_mat4 *m) {
    memset(m, 0, sizeof(*m));
    m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1.0f;
}

wb_o_mat4 wb_mat4_mul(wb_o_mat4 a, wb_o_mat4 b) {
    wb_o_mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a.m[k*4+j] * b.m[i*4+k];
            r.m[i*4+j] = s;
        }
    return r;
}

wb_o_vec4 wb_mat4_transform_vec4(wb_o_mat4 m, wb_o_vec4 v) {
    wb_o_vec4 r;
    r.x = m.m[0]*v.x + m.m[4]*v.y + m.m[8]*v.z + m.m[12]*v.w;
    r.y = m.m[1]*v.x + m.m[5]*v.y + m.m[9]*v.z + m.m[13]*v.w;
    r.z = m.m[2]*v.x + m.m[6]*v.y + m.m[10]*v.z + m.m[14]*v.w;
    r.w = m.m[3]*v.x + m.m[7]*v.y + m.m[11]*v.z + m.m[15]*v.w;
    return r;
}

wb_o_vec3 wb_mat4_transform_point(wb_o_mat4 m, wb_o_vec3 p) {
    wb_o_vec4 v = wb_mat4_transform_vec4(m, (wb_o_vec4){p.x, p.y, p.z, 1.0f});
    return (wb_o_vec3){v.x, v.y, v.z};
}

wb_o_vec3 wb_mat4_transform_dir(wb_o_mat4 m, wb_o_vec3 d) {
    wb_o_vec4 v = wb_mat4_transform_vec4(m, (wb_o_vec4){d.x, d.y, d.z, 0.0f});
    return (wb_o_vec3){v.x, v.y, v.z};
}

wb_o_mat4 wb_mat4_translate(float x, float y, float z) {
    wb_o_mat4 m;
    wb_mat4_identity(&m);
    m.m[12] = x; m.m[13] = y; m.m[14] = z;
    return m;
}

wb_o_mat4 wb_mat4_scale(float x, float y, float z) {
    wb_o_mat4 m;
    wb_mat4_identity(&m);
    m.m[0] = x; m.m[5] = y; m.m[10] = z;
    return m;
}

wb_o_mat4 wb_mat4_rotate_y(float angle) {
    wb_o_mat4 m;
    wb_mat4_identity(&m);
    float c = cosf(angle), s = sinf(angle);
    m.m[0] = c; m.m[8] = s;
    m.m[2] = -s; m.m[10] = c;
    return m;
}

wb_o_mat4 wb_mat4_rotate_x(float angle) {
    wb_o_mat4 m;
    wb_mat4_identity(&m);
    float c = cosf(angle), s = sinf(angle);
    m.m[5] = c; m.m[9] = -s;
    m.m[6] = s; m.m[10] = c;
    return m;
}

wb_o_mat4 wb_mat4_rotate_z(float angle) {
    wb_o_mat4 m;
    wb_mat4_identity(&m);
    float c = cosf(angle), s = sinf(angle);
    m.m[0] = c; m.m[4] = -s;
    m.m[1] = s; m.m[5] = c;
    return m;
}

wb_o_mat4 wb_mat4_perspective(float fov_y, float aspect, float near_c, float far_c) {
    wb_o_mat4 m;
    memset(&m, 0, sizeof(m));
    float t = 1.0f / tanf(fov_y * 0.5f);
    m.m[0] = t / aspect;
    m.m[5] = t;
    m.m[10] = (far_c + near_c) / (near_c - far_c);
    m.m[11] = -1.0f;
    m.m[14] = (2.0f * far_c * near_c) / (near_c - far_c);
    return m;
}

wb_o_mat4 wb_mat4_look_at(wb_o_vec3 eye, wb_o_vec3 target, wb_o_vec3 up) {
    wb_o_vec3 f = wb_vec3_norm(wb_vec3_sub(target, eye));
    wb_o_vec3 r = wb_vec3_norm(wb_vec3_cross(f, up));
    wb_o_vec3 u = wb_vec3_cross(r, f);
    wb_o_mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0]=r.x; m.m[4]=r.y; m.m[8]=r.z;
    m.m[1]=u.x; m.m[5]=u.y; m.m[9]=u.z;
    m.m[2]=-f.x; m.m[6]=-f.y; m.m[10]=-f.z;
    m.m[12]=-wb_vec3_dot(r,eye);
    m.m[13]=-wb_vec3_dot(u,eye);
    m.m[14]=wb_vec3_dot(f,eye);
    m.m[15]=1.0f;
    return m;
}

/* ================================================================
 * MESH FUNCTIONS
 * ================================================================ */

void wb_omesh_init(wb_omesh *mesh) {
    if (!mesh) return;
    memset(mesh, 0, sizeof(*mesh));
    for (int i = 0; i < WB_MAX_OVERLAY_BONES; i++)
        mesh->bones[i].parent = -1;
}

void wb_omesh_create_cube(wb_omesh *mesh, float size) {
    if (!mesh) return;
    wb_omesh_init(mesh);
    float s = size * 0.5f;
    wb_o_vec3 corners[8] = {
        {-s,-s,-s}, {s,-s,-s}, {s,s,-s}, {-s,s,-s},
        {-s,-s,s},  {s,-s,s},  {s,s,s},  {-s,s,s}
    };
    int face_verts[6][4] = {
        {0,1,2,3}, {5,4,7,6}, {4,5,1,0}, {3,2,6,7}, {4,0,3,7}, {1,5,6,2}
    };
    wb_o_vec3 normals[6] = {
        {0,0,-1}, {0,0,1}, {0,-1,0}, {0,1,0}, {-1,0,0}, {1,0,0}
    };
    for (int f = 0; f < 6; f++) {
        for (int v = 0; v < 4; v++) {
            int vi = mesh->n_verts;
            mesh->vertices[vi].pos = corners[face_verts[f][v]];
            mesh->vertices[vi].normal = normals[f];
            mesh->vertices[vi].u = (v == 1 || v == 2) ? 1.0f : 0.0f;
            mesh->vertices[vi].v = (v == 2 || v == 3) ? 1.0f : 0.0f;
            mesh->vertices[vi].bone_ids[0] = 0;
            mesh->vertices[vi].bone_weights[0] = 1.0f;
            mesh->n_verts++;
        }
        int base = mesh->n_verts - 4;
        mesh->faces[mesh->n_faces].v[0] = base;
        mesh->faces[mesh->n_faces].v[1] = base+1;
        mesh->faces[mesh->n_faces].v[2] = base+2;
        mesh->n_faces++;
        mesh->faces[mesh->n_faces].v[0] = base;
        mesh->faces[mesh->n_faces].v[1] = base+2;
        mesh->faces[mesh->n_faces].v[2] = base+3;
        mesh->n_faces++;
    }
    mesh->n_bones = 1;
    strcpy(mesh->bones[0].name, "root");
    mesh->bones[0].parent = -1;
    wb_mat4_identity(&mesh->bones[0].bind_pose);
    wb_mat4_identity(&mesh->bones[0].inv_bind);
}

void wb_omesh_create_humanoid(wb_omesh *mesh) {
    if (!mesh) return;
    wb_omesh_init(mesh);
    float part_size = 0.15f;
    wb_omesh_create_cube(mesh, part_size);
    for (int i = 0; i < mesh->n_verts; i++)
        mesh->vertices[i].pos.y += 1.6f;
    float body_s = part_size * 1.5f;
    float bs = body_s * 0.5f;
    wb_o_vec3 body_corners[8] = {
        {-bs,1.0f,-bs},{bs,1.0f,-bs},{bs,1.4f,-bs},{-bs,1.4f,-bs},
        {-bs,1.0f,bs},{bs,1.0f,bs},{bs,1.4f,bs},{-bs,1.4f,bs}
    };
    int face_verts[6][4] = {
        {0,1,2,3},{5,4,7,6},{4,5,1,0},{3,2,6,7},{4,0,3,7},{1,5,6,2}
    };
    wb_o_vec3 normals[6] = {{0,0,-1},{0,0,1},{0,-1,0},{0,1,0},{-1,0,0},{1,0,0}};
    for (int f = 0; f < 6; f++) {
        for (int v = 0; v < 4; v++) {
            int vi = mesh->n_verts;
            mesh->vertices[vi].pos = body_corners[face_verts[f][v]];
            mesh->vertices[vi].normal = normals[f];
            mesh->vertices[vi].u = (v==1||v==2)?1:0;
            mesh->vertices[vi].v = (v==2||v==3)?1:0;
            mesh->vertices[vi].bone_ids[0] = 1;
            mesh->vertices[vi].bone_weights[0] = 1.0f;
            mesh->n_verts++;
        }
        int base = mesh->n_verts - 4;
        mesh->faces[mesh->n_faces].v[0] = base;
        mesh->faces[mesh->n_faces].v[1] = base+1;
        mesh->faces[mesh->n_faces].v[2] = base+2;
        mesh->n_faces++;
        mesh->faces[mesh->n_faces].v[0] = base;
        mesh->faces[mesh->n_faces].v[1] = base+2;
        mesh->faces[mesh->n_faces].v[2] = base+3;
        mesh->n_faces++;
    }
    mesh->n_bones = 6;
    strcpy(mesh->bones[0].name, "head"); mesh->bones[0].parent = 1;
    mesh->bones[0].bind_pose = wb_mat4_translate(0, 1.6f, 0);
    strcpy(mesh->bones[1].name, "spine"); mesh->bones[1].parent = -1;
    wb_mat4_identity(&mesh->bones[1].bind_pose);
    strcpy(mesh->bones[2].name, "left_arm"); mesh->bones[2].parent = 1;
    mesh->bones[2].bind_pose = wb_mat4_translate(-0.3f, 1.3f, 0);
    strcpy(mesh->bones[3].name, "right_arm"); mesh->bones[3].parent = 1;
    mesh->bones[3].bind_pose = wb_mat4_translate(0.3f, 1.3f, 0);
    strcpy(mesh->bones[4].name, "left_leg"); mesh->bones[4].parent = 1;
    mesh->bones[4].bind_pose = wb_mat4_translate(-0.15f, 0.5f, 0);
    strcpy(mesh->bones[5].name, "right_leg"); mesh->bones[5].parent = 1;
    mesh->bones[5].bind_pose = wb_mat4_translate(0.15f, 0.5f, 0);
    for (int i = 0; i < mesh->n_bones; i++)
        wb_mat4_identity(&mesh->bones[i].inv_bind);
}

/* ================================================================
 * SKELETAL ANIMATION
 * ================================================================ */

void wb_anim_init(wb_animation *anim) {
    if (!anim) return;
    memset(anim, 0, sizeof(*anim));
    anim->fps = 30.0f;
}

void wb_anim_create_walk(wb_animation *anim) {
    if (!anim) return;
    wb_anim_init(anim);
    strcpy(anim->name, "walk");
    anim->duration = 1.0f;
    anim->fps = 30.0f;
    anim->n_tracks = 4;
    const char *bone_names[4] = {"left_arm", "right_arm", "left_leg", "right_leg"};
    for (int t = 0; t < 4; t++) {
        strcpy(anim->tracks[t].bone_name, bone_names[t]);
        anim->tracks[t].n_keyframes = 5;
        float swing = (t < 2) ? 0.4f : 0.3f;
        int mirror = (t % 2 == 0) ? 1 : -1;
        for (int k = 0; k < 5; k++) {
            float time = k * 0.25f;
            anim->tracks[t].keyframes[k].time = time;
            anim->tracks[t].keyframes[k].translation = (wb_o_vec3){0,0,0};
            anim->tracks[t].keyframes[k].scale = (wb_o_vec3){1,1,1};
            float angle = sinf(time * 2.0f * M_PI) * swing * mirror;
            anim->tracks[t].keyframes[k].rotation[0] = 0;
            anim->tracks[t].keyframes[k].rotation[1] = 0;
            anim->tracks[t].keyframes[k].rotation[2] = sinf(angle * 0.5f);
            anim->tracks[t].keyframes[k].rotation[3] = cosf(angle * 0.5f);
        }
    }
}

void wb_anim_create_dance(wb_animation *anim) {
    if (!anim) return;
    wb_anim_init(anim);
    strcpy(anim->name, "dance");
    anim->duration = 2.0f;
    anim->fps = 30.0f;
    anim->n_tracks = 6;
    const char *bone_names[6] = {"head", "spine", "left_arm", "right_arm", "left_leg", "right_leg"};
    for (int t = 0; t < 6; t++) {
        strcpy(anim->tracks[t].bone_name, bone_names[t]);
        anim->tracks[t].n_keyframes = 9;
        float amp = 0.5f + t * 0.1f;
        for (int k = 0; k < 9; k++) {
            float time = k * 0.25f;
            anim->tracks[t].keyframes[k].time = time;
            anim->tracks[t].keyframes[k].translation = (wb_o_vec3){
                sinf(time * 4.0f * M_PI) * 0.05f,
                sinf(time * 2.0f * M_PI) * 0.1f, 0
            };
            anim->tracks[t].keyframes[k].scale = (wb_o_vec3){1,1,1};
            float angle = sinf(time * 2.0f * M_PI + t * 0.5f) * amp;
            anim->tracks[t].keyframes[k].rotation[0] = sinf(angle * 0.3f);
            anim->tracks[t].keyframes[k].rotation[1] = 0;
            anim->tracks[t].keyframes[k].rotation[2] = sinf(angle * 0.5f);
            anim->tracks[t].keyframes[k].rotation[3] = cosf(angle * 0.5f);
        }
    }
}

void wb_anim_evaluate(wb_animation *anim, float time, wb_omesh *mesh, wb_o_mat4 *bone_matrices) {
    if (!anim || !mesh || !bone_matrices) return;
    float t = fmodf(time, anim->duration);
    if (t < 0) t += anim->duration;
    for (int b = 0; b < mesh->n_bones; b++) {
        wb_anim_track *track = NULL;
        for (int tr = 0; tr < anim->n_tracks; tr++) {
            if (strcmp(anim->tracks[tr].bone_name, mesh->bones[b].name) == 0) {
                track = &anim->tracks[tr]; break;
            }
        }
        if (track && track->n_keyframes >= 2) {
            int k0 = 0, k1 = 1;
            for (int k = 0; k < track->n_keyframes - 1; k++) {
                if (t >= track->keyframes[k].time && t < track->keyframes[k+1].time) {
                    k0 = k; k1 = k + 1; break;
                }
            }
            float t0 = track->keyframes[k0].time;
            float t1 = track->keyframes[k1].time;
            float blend = (t1 > t0) ? (t - t0) / (t1 - t0) : 0;
            wb_o_vec3 trans = wb_vec3_add(
                wb_vec3_scale(track->keyframes[k0].translation, 1.0f - blend),
                wb_vec3_scale(track->keyframes[k1].translation, blend)
            );
            wb_o_mat4 T = wb_mat4_translate(trans.x, trans.y, trans.z);
            float q[4];
            for (int i = 0; i < 4; i++)
                q[i] = wb_lerp(track->keyframes[k0].rotation[i],
                               track->keyframes[k1].rotation[i], blend);
            float qlen = sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
            if (qlen > 1e-6f) { q[0]/=qlen; q[1]/=qlen; q[2]/=qlen; q[3]/=qlen; }
            float x=q[0], y=q[1], z=q[2], w=q[3];
            wb_o_mat4 R;
            memset(&R, 0, sizeof(R));
            R.m[0]=1-2*(y*y+z*z); R.m[4]=2*(x*y-z*w);   R.m[8]=2*(x*z+y*w);
            R.m[1]=2*(x*y+z*w);   R.m[5]=1-2*(x*x+z*z); R.m[9]=2*(y*z-x*w);
            R.m[2]=2*(x*z-y*w);   R.m[6]=2*(y*z+x*w);   R.m[10]=1-2*(x*x+y*y);
            R.m[15]=1.0f;
            bone_matrices[b] = wb_mat4_mul(T, R);
        } else {
            bone_matrices[b] = mesh->bones[b].bind_pose;
        }
    }
}

/* ================================================================
 * LIP-SYNC
 * ================================================================ */

void wb_lipsync_init(wb_lipsync *ls) {
    if (!ls) return;
    memset(ls, 0, sizeof(*ls));
    ls->blend_speed = 15.0f;
}

void wb_lipsync_set_viseme(wb_lipsync *ls, int phoneme_type) {
    if (!ls) return;
    int viseme;
    switch (phoneme_type) {
        case 0: viseme = VISEME_AH; break;
        case 1: viseme = VISEME_EE; break;
        case 2: viseme = VISEME_EE; break;
        case 3: viseme = VISEME_OH; break;
        case 4: viseme = VISEME_OO; break;
        case 5: viseme = VISEME_MBP; break;
        case 6: viseme = VISEME_FV; break;
        case 7: viseme = VISEME_TH; break;
        default: viseme = VISEME_REST; break;
    }
    if (viseme != ls->target_viseme) {
        ls->target_viseme = viseme;
        ls->blend = 0.0f;
    }
}

void wb_lipsync_update(wb_lipsync *ls, float dt) {
    if (!ls) return;
    if (ls->blend < 1.0f) {
        ls->blend += ls->blend_speed * dt;
        if (ls->blend > 1.0f) ls->blend = 1.0f;
        ls->current_viseme = ls->target_viseme;
    }
    switch (ls->current_viseme) {
        case VISEME_REST: ls->mouth_open = 0.0f; ls->mouth_wide = 0.5f; break;
        case VISEME_AH:   ls->mouth_open = 0.8f; ls->mouth_wide = 0.6f; break;
        case VISEME_EE:   ls->mouth_open = 0.3f; ls->mouth_wide = 0.9f; break;
        case VISEME_OH:   ls->mouth_open = 0.7f; ls->mouth_wide = 0.3f; break;
        case VISEME_OO:   ls->mouth_open = 0.5f; ls->mouth_wide = 0.2f; break;
        case VISEME_FV:   ls->mouth_open = 0.1f; ls->mouth_wide = 0.6f; break;
        case VISEME_MBP:  ls->mouth_open = 0.0f; ls->mouth_wide = 0.5f; break;
        case VISEME_TH:   ls->mouth_open = 0.2f; ls->mouth_wide = 0.6f; break;
        case VISEME_L:    ls->mouth_open = 0.3f; ls->mouth_wide = 0.5f; break;
        case VISEME_W:    ls->mouth_open = 0.3f; ls->mouth_wide = 0.2f; break;
        default:          ls->mouth_open = 0.0f; ls->mouth_wide = 0.5f; break;
    }
}

/* ================================================================
 * PARTICLE SYSTEM
 * ================================================================ */

void wb_particles_init(wb_particle_system *ps, wb_particle_config *config) {
    if (!ps || !config) return;
    memcpy(&ps->config, config, sizeof(*config));
    ps->n_active = 0;
    ps->emit_accum = 0;
}

void wb_particles_update(wb_particle_system *ps, float dt) {
    if (!ps) return;
    ps->emit_accum += ps->config.emit_rate * dt;
    while (ps->emit_accum >= 1.0f && ps->n_active < 256) {
        ps->emit_accum -= 1.0f;
        int i = ps->n_active++;
        float angle = ps->config.angle + ((float)rand()/RAND_MAX - 0.5f) * ps->config.spread;
        float speed = wb_lerp(ps->config.vel_min, ps->config.vel_max, (float)rand()/RAND_MAX);
        float life = wb_lerp(ps->config.life_min, ps->config.life_max, (float)rand()/RAND_MAX);
        ps->particles[i].x = ps->config.emit_x;
        ps->particles[i].y = ps->config.emit_y;
        ps->particles[i].vx = cosf(angle) * speed;
        ps->particles[i].vy = sinf(angle) * speed;
        ps->particles[i].life = life;
        ps->particles[i].max_life = life;
        ps->particles[i].size = ps->config.size_start;
        ps->particles[i].color = ps->config.color_start;
        ps->particles[i].active = 1;
    }
    for (int i = 0; i < ps->n_active; i++) {
        if (!ps->particles[i].active) continue;
        ps->particles[i].x += ps->particles[i].vx * dt;
        ps->particles[i].y += ps->particles[i].vy * dt;
        ps->particles[i].vy += ps->config.gravity * dt;
        ps->particles[i].life -= dt;
        float t = 1.0f - ps->particles[i].life / ps->particles[i].max_life;
        ps->particles[i].size = wb_lerp(ps->config.size_start, ps->config.size_end, t);
        if (ps->particles[i].life <= 0) ps->particles[i].active = 0;
    }
    int write = 0;
    for (int i = 0; i < ps->n_active; i++) {
        if (ps->particles[i].active) {
            if (write != i) ps->particles[write] = ps->particles[i];
            write++;
        }
    }
    ps->n_active = write;
}

/* ================================================================
 * COMPOSITION / LAYER STACK
 * ================================================================ */

void wb_comp_init(wb_comp *comp, int w, int h, float duration, float fps) {
    if (!comp) return;
    memset(comp, 0, sizeof(*comp));
    comp->width = w;
    comp->height = h;
    comp->duration = duration;
    comp->fps = fps;
    comp->camera_pos = (wb_o_vec3){0, 0, 5};
    comp->camera_target = (wb_o_vec3){0, 0, 0};
    comp->camera_up = (wb_o_vec3){0, 1, 0};
    comp->camera_fov = 60.0f * M_PI / 180.0f;
    comp->output = (uint8_t *)calloc(w * h * 4, 1);
}

void wb_comp_free(wb_comp *comp) {
    if (!comp) return;
    free(comp->output);
    for (int i = 0; i < comp->n_layers; i++)
        if (comp->layers[i].buffer)
            free(comp->layers[i].buffer);
}

int wb_comp_add_layer(wb_comp *comp, int type, const char *name) {
    if (!comp || comp->n_layers >= WB_MAX_LAYERS) return -1;
    int idx = comp->n_layers++;
    wb_layer *layer = &comp->layers[idx];
    memset(layer, 0, sizeof(*layer));
    strncpy(layer->name, name, 63);
    layer->type = type;
    layer->visible = 1;
    layer->blend_mode = WB_BLEND_NORMAL;
    layer->transform.opacity = 1.0f;
    layer->transform.scale_x = 1.0f;
    layer->transform.scale_y = 1.0f;
    layer->transform.scale_z = 1.0f;
    layer->parent = -1;
    layer->in_point = 0;
    layer->out_point = comp->duration;
    layer->time_stretch = 1.0f;
    return idx;
}

/* Blend a single channel */
static uint8_t wb_blend_channel(uint8_t dst, uint8_t src, int mode) {
    float d = dst / 255.0f;
    float s = src / 255.0f;
    float r;
    switch (mode) {
        case WB_BLEND_NORMAL:     r = s; break;
        case WB_BLEND_MULTIPLY:   r = d * s; break;
        case WB_BLEND_SCREEN:     r = 1 - (1-d)*(1-s); break;
        case WB_BLEND_OVERLAY:    r = d < 0.5f ? 2*d*s : 1-2*(1-d)*(1-s); break;
        case WB_BLEND_SOFT_LIGHT: r = d + (2*s-1)*(d-d*d); break;
        case WB_BLEND_HARD_LIGHT: r = s < 0.5f ? 2*d*s : 1-2*(1-d)*(1-s); break;
        case WB_BLEND_DIFFERENCE: r = fabsf(d - s); break;
        case WB_BLEND_EXCLUSION:  r = d + s - 2*d*s; break;
        case WB_BLEND_DARKEN:     r = d < s ? d : s; break;
        case WB_BLEND_LIGHTEN:    r = d > s ? d : s; break;
        case WB_BLEND_COLOR_DODGE: r = d / (1-s + 1e-6f); break;
        case WB_BLEND_COLOR_BURN:  r = 1 - (1-d)/(s + 1e-6f); break;
        case WB_BLEND_ADD:        r = d + s; break;
        case WB_BLEND_SUBTRACT:   r = d - s; break;
        case WB_BLEND_DIVIDE:     r = d / (s + 1e-6f); break;
        default: r = s; break;
    }
    return (uint8_t)(wb_clamp(r, 0, 1) * 255);
}

void wb_comp_blend_layer(wb_comp *comp, int layer_idx) {
    if (!comp || layer_idx < 0 || layer_idx >= comp->n_layers) return;
    wb_layer *layer = &comp->layers[layer_idx];
    if (!layer->visible || !layer->buffer) return;
    int w = comp->width, h = comp->height;
    float opacity = layer->transform.opacity;
    int mode = layer->blend_mode;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * 4;
            uint8_t sa = (uint8_t)(layer->buffer[idx+3] * opacity);
            if (sa == 0) continue;
            uint8_t da = comp->output[idx+3];
            if (mode == WB_BLEND_NORMAL && sa == 255) {
                comp->output[idx]   = layer->buffer[idx];
                comp->output[idx+1] = layer->buffer[idx+1];
                comp->output[idx+2] = layer->buffer[idx+2];
                comp->output[idx+3] = 255;
            } else {
                for (int c = 0; c < 3; c++) {
                    uint8_t blended = wb_blend_channel(comp->output[idx+c], layer->buffer[idx+c], mode);
                    comp->output[idx+c] = (uint8_t)(blended * sa / 255.0f + comp->output[idx+c] * (1.0f - sa/255.0f));
                }
                comp->output[idx+3] = (uint8_t)(sa + da * (1.0f - sa/255.0f));
            }
        }
    }
}

/* ================================================================
 * SOFTWARE RASTERIZER
 * ================================================================ */

void wb_rasterize_mesh(uint8_t *buffer, int w, int h, wb_omesh *mesh,
                        wb_o_mat4 *bone_matrices, wb_o_mat4 *mvp, uint32_t color) {
    if (!buffer || !mesh || !mvp) return;
    float cr = ((color >> 0) & 0xFF) / 255.0f;
    float cg = ((color >> 8) & 0xFF) / 255.0f;
    float cb = ((color >> 16) & 0xFF) / 255.0f;
    float ca = ((color >> 24) & 0xFF) / 255.0f;
    float *depth = (float *)calloc(w * h, sizeof(float));
    if (!depth) return;
    for (int i = 0; i < w*h; i++) depth[i] = 1e10f;
    for (int f = 0; f < mesh->n_faces; f++) {
        wb_o_face *face = &mesh->faces[f];
        wb_o_vec4 clip[3];
        for (int v = 0; v < 3; v++) {
            wb_o_vertex *vert = &mesh->vertices[face->v[v]];
            wb_o_vec3 pos = vert->pos;
            if (bone_matrices && vert->bone_ids[0] >= 0 && vert->bone_ids[0] < WB_MAX_OVERLAY_BONES)
                pos = wb_mat4_transform_point(bone_matrices[vert->bone_ids[0]], pos);
            clip[v] = wb_mat4_transform_vec4(*mvp, (wb_o_vec4){pos.x, pos.y, pos.z, 1.0f});
        }
        wb_o_vec3 ndc[3];
        int valid = 1;
        for (int v = 0; v < 3; v++) {
            if (clip[v].w < 1e-6f) { valid = 0; break; }
            ndc[v].x = clip[v].x / clip[v].w;
            ndc[v].y = clip[v].y / clip[v].w;
            ndc[v].z = clip[v].z / clip[v].w;
        }
        if (!valid) continue;
        int sx[3], sy[3];
        for (int v = 0; v < 3; v++) {
            sx[v] = (int)((ndc[v].x * 0.5f + 0.5f) * w);
            sy[v] = (int)((1.0f - (ndc[v].y * 0.5f + 0.5f)) * h);
        }
        int min_x = sx[0], max_x = sx[0], min_y = sy[0], max_y = sy[0];
        for (int v = 1; v < 3; v++) {
            if (sx[v] < min_x) min_x = sx[v];
            if (sx[v] > max_x) max_x = sx[v];
            if (sy[v] < min_y) min_y = sy[v];
            if (sy[v] > max_y) max_y = sy[v];
        }
        if (min_x < 0) min_x = 0; if (min_y < 0) min_y = 0;
        if (max_x >= w) max_x = w-1; if (max_y >= h) max_y = h-1;
        for (int py = min_y; py <= max_y; py++) {
            for (int px = min_x; px <= max_x; px++) {
                float d1 = (px-sx[0])*(sy[1]-sy[0]) - (py-sy[0])*(sx[1]-sx[0]);
                float d2 = (px-sx[1])*(sy[2]-sy[1]) - (py-sy[1])*(sx[2]-sx[1]);
                float d3 = (px-sx[2])*(sy[0]-sy[2]) - (py-sy[2])*(sx[0]-sx[2]);
                float area = (sx[1]-sx[0])*(sy[2]-sy[0]) - (sx[2]-sx[0])*(sy[1]-sy[0]);
                if (fabsf(area) < 1e-6f) continue;
                float u = d2/area, v = d3/area, wc = 1.0f - u - v;
                if (u < 0 || v < 0 || wc < 0) continue;
                float z = u*ndc[0].z + v*ndc[1].z + wc*ndc[2].z;
                if (z < depth[py*w+px]) {
                    depth[py*w+px] = z;
                    int i = (py*w+px)*4;
                    buffer[i]=(uint8_t)(cr*255); buffer[i+1]=(uint8_t)(cg*255);
                    buffer[i+2]=(uint8_t)(cb*255); buffer[i+3]=(uint8_t)(ca*255);
                }
            }
        }
    }
    free(depth);
}

/* ================================================================
 * WIGGLE / AUDIO REACTIVE
 * ================================================================ */

void wb_wiggle_update(wb_layer *layer, float dt) {
    if (!layer) return;
    layer->wiggle_time += dt;
    float t = layer->wiggle_time * layer->wiggle_freq;
    layer->transform.pos_x += sinf(t * 1.7f) * layer->wiggle_amp * dt;
    layer->transform.pos_y += cosf(t * 2.3f) * layer->wiggle_amp * dt;
    layer->transform.rot_z += sinf(t * 3.1f) * layer->wiggle_amp * dt * 0.5f;
}

void wb_overlay_audio_reactive_update(wb_layer *layer, float audio_level) {
    if (!layer || !layer->audio_reactive) return;
    float t = wb_clamp(audio_level, 0, 1);
    float scale = wb_lerp(layer->audio_scale_min, layer->audio_scale_max, t);
    layer->transform.scale_x = scale;
    layer->transform.scale_y = scale;
    layer->transform.rot_z += t * layer->audio_rotation_scale * 0.01f;
}
