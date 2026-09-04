/* wb_composition.c — Composition Timeline System (R103).
 * Types defined in wbus_compositor.h — this file is implementations only.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wbus/wbus_compositor.h"

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* Keyframe track */
void wb_comp_kf_init(wb_comp_kf_track *t, float default_val) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->default_value = default_val;
}

int wb_comp_kf_add(wb_comp_kf_track *t, float time, float value, int interp) {
    if (!t || t->n_keys >= MAX_KEYFRAMES) return -1;
    int idx = t->n_keys++;
    t->keys[idx].time = time;
    t->keys[idx].value = value;
    t->keys[idx].interpolation = interp;
    t->keys[idx].tangent_in = 0;
    t->keys[idx].tangent_out = 0;
    /* Sort by time (insertion sort) */
    for (int i = idx; i > 0 && t->keys[i-1].time > t->keys[i].time; i--) {
        wb_comp_keyframe tmp = t->keys[i];
        t->keys[i] = t->keys[i-1];
        t->keys[i-1] = tmp;
    }
    return idx;
}

float wb_comp_kf_eval(const wb_comp_kf_track *t, float time) {
    if (!t || t->n_keys == 0) return t ? t->default_value : 0;
    if (time <= t->keys[0].time) return t->keys[0].value;
    if (time >= t->keys[t->n_keys-1].time) return t->keys[t->n_keys-1].value;
    int k0 = 0, k1 = t->n_keys - 1;
    for (int i = 0; i < t->n_keys - 1; i++) {
        if (time >= t->keys[i].time && time < t->keys[i+1].time) { k0 = i; k1 = i+1; break; }
    }
    float t0 = t->keys[k0].time, t1 = t->keys[k1].time;
    float v0 = t->keys[k0].value, v1 = t->keys[k1].value;
    if (t1 <= t0) return v0;
    float lt = (time - t0) / (t1 - t0);
    switch (t->keys[k0].interpolation) {
        case 0: return v0;
        case 1: return v0 + (v1 - v0) * lt;
        case 3: { float s = lt*lt*(3-2*lt); return v0 + (v1-v0)*s; }
        default: return v0 + (v1 - v0) * lt;
    }
}

/* 3D transform */
void wb_comp_transform_init(wb_comp_transform_3d *t) {
    if (!t) return;
    t->x=t->y=t->z=0; t->rx=t->ry=t->rz=0;
    t->sx=t->sy=t->sz=1.0f; t->ax=t->ay=t->az=0; t->opacity=1.0f;
}

void wb_comp_transform_apply(const wb_comp_transform_3d *t, float px, float py, float *ox, float *oy) {
    if (!t||!ox||!oy) return;
    float x=px-t->ax, y=py-t->ay;
    x*=t->sx; y*=t->sy;
    float cz=cosf(t->rz), sz=sinf(t->rz);
    *ox = x*cz - y*sz + t->ax + t->x;
    *oy = x*sz + y*cz + t->ay + t->y;
}

/* Track matte */
void wb_comp_matte_init(wb_comp_track_matte *m) {
    if (!m) return;
    m->matte_layer=-1; m->matte_type=0; m->preserve_alpha=1;
}

float wb_comp_matte_apply(const wb_comp_track_matte *m, float src_a, float matte_a, float matte_l) {
    if (!m||m->matte_layer<0) return src_a;
    float v;
    switch(m->matte_type) {
        case 1: v=matte_a; break;
        case 2: v=matte_l; break;
        case 3: v=1.0f-matte_a; break;
        case 4: v=1.0f-matte_l; break;
        default: return src_a;
    }
    return m->preserve_alpha ? src_a*v : v;
}

/* Composition */
void wb_comp_timeline_init(wb_comp_comp *c, int w, int h, float fps, float duration) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->width=w; c->height=h; c->fps=fps>0?fps:30; c->duration=duration>0?duration:10;
    c->bpm=120; c->bg_a=1;
    c->camera.fov=60; c->camera.near_plane=0.1f; c->camera.far_plane=1000;
    c->output=(uint8_t*)calloc(w*h*4,1);
}

void wb_comp_timeline_free(wb_comp_comp *c) {
    if (!c) return;
    free(c->output);
}

int wb_comp_timeline_add_track(wb_comp_comp *c, int type, const char *name) {
    if (!c||c->n_tracks>=16) return -1;
    int idx=c->n_tracks++;
    wb_comp_track *t=&c->tracks[idx];
    memset(t,0,sizeof(*t));
    t->active=1; t->type=type; strncpy(t->name,name,63);
    t->volume=1.0f; t->parent=-1;
    return idx;
}

wb_comp_track_clip* wb_comp_timeline_get_clip_at(wb_comp_comp *c, int ti, float time) {
    if (!c||ti<0||ti>=c->n_tracks) return NULL;
    wb_comp_track *t=&c->tracks[ti];
    for (int i=0;i<t->n_clips;i++) {
        if (t->clips[i].active && time>=t->clips[i].start_time && time<t->clips[i].start_time+t->clips[i].duration)
            return &t->clips[i];
    }
    return NULL;
}

void wb_comp_timeline_eval_transform(wb_comp_track_clip *clip, float time, wb_comp_transform_3d *out) {
    if (!clip||!out) return;
    float lt=time-clip->start_time;
    wb_comp_transform_init(out);
    out->x=wb_comp_kf_eval(&clip->pos_x,lt);
    out->y=wb_comp_kf_eval(&clip->pos_y,lt);
    out->z=wb_comp_kf_eval(&clip->pos_z,lt);
    out->rx=wb_comp_kf_eval(&clip->rot_x,lt);
    out->ry=wb_comp_kf_eval(&clip->rot_y,lt);
    out->rz=wb_comp_kf_eval(&clip->rot_z,lt);
    out->sx=wb_comp_kf_eval(&clip->scale_x,lt);
    out->sy=wb_comp_kf_eval(&clip->scale_y,lt);
    out->opacity=wb_comp_kf_eval(&clip->opacity,lt);
}

void wb_comp_timeline_render_frame(wb_comp_comp *c, float time) {
    if (!c||!c->output) return;
    for (int i=0;i<c->width*c->height;i++) {
        c->output[i*4]=(uint8_t)(c->bg_r*255);
        c->output[i*4+1]=(uint8_t)(c->bg_g*255);
        c->output[i*4+2]=(uint8_t)(c->bg_b*255);
        c->output[i*4+3]=(uint8_t)(c->bg_a*255);
    }
}
