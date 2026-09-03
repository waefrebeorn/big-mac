/* wb_ytp_video_fx.c — Video Poopisms Engine + YTPMV Pipeline (R099).
 * Closing 30 gaps from the R094 gap database.
 * Types declared in wbus_compositor.h — this file is implementations only.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wbus/wbus_compositor.h"

#ifndef M_PI
#define M_PI 3.14159265358979
#endif



/* ================================================================
 * KEYFRAME INTERPOLATION (K1-K16)
 * ================================================================ */

static float kf_eval(float t, int type, float p1, float p2, float p3) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    switch (type) {
        case 0: return 0; /* HOLD */
        case 1: return t; /* LINEAR */
        case 2: { /* BEZIER */
            float u = 1 - t;
            return 3*u*u*t*p1 + 3*u*t*t*p2 + t*t*t;
        }
        case 3: { /* HERMITE */
            float t2=t*t, t3=t2*t;
            return (2*t3-3*t2+1)*0 + (t3-2*t2+t)*p1 + (-2*t3+3*t2)*1 + (t3-t2)*p2;
        }
        case 14: return 0; /* STEP */
        case 4: return t*t; /* EASE_IN */
        case 5: return 1-(1-t)*(1-t); /* EASE_OUT */
        case 6: return t < 0.5f ? 2*t*t : 1 - powf(-2*t+2,2)/2; /* EASE_INOUT */
        case 7: { /* ELASTIC */
            float c4 = (2*M_PI)/3;
            return t==0?0:t==1?1:powf(2,-10*t)*sinf((t*10-0.75f)*c4)+1;
        }
        case 8: { /* BOUNCE */
            float n1=7.5625f, d1=2.75f;
            if (t<1/d1) return n1*t*t;
            else if (t<2/d1) { t-=1.5f/d1; return n1*t*t+0.75f; }
            else if (t<2.5f/d1) { t-=2.25f/d1; return n1*t*t+0.9375f; }
            else { t-=2.625f/d1; return n1*t*t+0.984375f; }
        }
        case 9: { /* BACK */
            float c1=1.70158f, c3=c1+1;
            return c3*t*t*t - c1*t*t;
        }
        case 10: return t==0?0:powf(2,10*(t-1)); /* EXPONENTIAL */
        case 11: return t==0?0:logf(1+9*t)/logf(10); /* LOGARITHMIC */
        case 12: return t*t*(3-2*t); /* SCURVE */
        case 13: { /* TCB */
            float tension=p1, cont=p2, bias=p3;
            float h1=(1-tension)*(1-cont)*(1+bias)/2;
            float h2=(1-tension)*(1+cont)*(1-bias)/2;
            float h3=(1-tension)*(1-cont)*(1-bias)/2;
            float h4=(1-tension)*(1+cont)*(1+bias)/2;
            return (h1*(1-t)+h2*t+h3*(1-t)+h4*t);
        }
        default: return t;
    }
}

float wb_kf_interpolate(float t, int type, float p1, float p2, float p3) {
    return kf_eval(t, type, p1, p2, p3);
}

/* ================================================================
 * FADE CURVES
 * ================================================================ */

float wb_fade_eval(float t, int type) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    switch (type) {
        case 0: return t; /* LINEAR */
        case 1: return powf(2, 10*(t-1)); /* EXPONENTIAL */
        case 2: return logf(1+9*t)/logf(10); /* LOGARITHMIC */
        case 3: return t*t*(3-2*t); /* SCURVE */
        default: return t;
    }
}

/* ================================================================
 * STUTTER LOOP PLUS
 * ================================================================ */

void wb_stutter_plus_init(wb_stutter_plus *sp, int n_repeats) {
    if (!sp) return;
    memset(sp, 0, sizeof(*sp));
    sp->n_repeats = n_repeats > 0 ? n_repeats : 4;
    sp->fx_per_repeat = 5;
}

void wb_stutter_plus_apply(wb_stutter_plus *sp, uint8_t *frame, int w, int h, int repeat_idx) {
    if (!sp || !frame) return;
    int fx = repeat_idx % sp->fx_per_repeat;
    int n_pixels = w * h;
    switch (fx) {
        case 0: break;
        case 1:
            for (int i = 0; i < n_pixels; i++) {
                int o = i*4;
                frame[o]=255-frame[o]; frame[o+1]=255-frame[o+1]; frame[o+2]=255-frame[o+2];
            }
            break;
        case 2:
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int o = (y*w+x)*4;
                    int rs = (y*w+(x+3<w?x+3:x))*4;
                    int bs = (y*w+(x-3>=0?x-3:x))*4;
                    frame[o] = frame[rs];
                    frame[o+2] = frame[bs+2];
                }
            break;
        case 3:
            for (int i = 0; i < n_pixels; i++) {
                int o = i*4;
                frame[o]=(frame[o]/64)*64; frame[o+1]=(frame[o+1]/64)*64; frame[o+2]=(frame[o+2]/64)*64;
            }
            break;
        case 4:
            for (int i = 0; i < n_pixels; i++) {
                int o = i*4;
                float r=(frame[o]-128)*1.5f+128, g=(frame[o+1]-128)*1.5f+128, b=(frame[o+2]-128)*1.5f+128;
                frame[o]=(uint8_t)(r<0?0:r>255?255:r);
                frame[o+1]=(uint8_t)(g<0?0:g>255?255:g);
                frame[o+2]=(uint8_t)(b<0?0:b>255?255:b);
            }
            break;
    }
}

/* ================================================================
 * STROBE / FLASH / IMPACT FRAME
 * ================================================================ */

void wb_strobe_init(wb_strobe_state *st, int interval) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->strobe_interval = interval > 0 ? interval : 4;
}

int wb_strobe_tick(wb_strobe_state *st) {
    if (!st) return 0;
    st->frame_count++;
    return (st->frame_count % st->strobe_interval) == 0;
}

void wb_strobe_apply(uint8_t *frame, int w, int h, int color) {
    if (!frame) return;
    uint8_t r, g, b;
    switch (color) {
        case 0: r=255; g=255; b=255; break;
        case 1: r=0; g=0; b=0; break;
        case 2: r=255; g=0; b=0; break;
        default: r=255; g=255; b=255; break;
    }
    int n = w * h * 4;
    for (int i = 0; i < n; i += 4) {
        frame[i]=r; frame[i+1]=g; frame[i+2]=b; frame[i+3]=255;
    }
}

void wb_invert_flash_apply(uint8_t *frame, int w, int h) {
    if (!frame) return;
    int n = w * h;
    for (int i = 0; i < n; i++) {
        int o = i*4;
        frame[o]=255-frame[o]; frame[o+1]=255-frame[o+1]; frame[o+2]=255-frame[o+2];
    }
}

/* ================================================================
 * FRAME FREEZE / HOLD
 * ================================================================ */

void wb_freeze_init(wb_frame_freeze *fz) {
    if (!fz) return;
    memset(fz, 0, sizeof(*fz));
}

void wb_freeze_capture(wb_frame_freeze *fz, uint8_t *frame, int w, int h) {
    if (!fz || !frame) return;
    if (!fz->frozen_frame || fz->w != w || fz->h != h) {
        free(fz->frozen_frame);
        fz->frozen_frame = (uint8_t *)malloc(w * h * 4);
        fz->w = w; fz->h = h;
    }
    memcpy(fz->frozen_frame, frame, w * h * 4);
    fz->is_frozen = 1;
}

void wb_freeze_hold(wb_frame_freeze *fz, int n_frames) {
    if (!fz) return;
    fz->hold_frames_remaining = n_frames;
}

int wb_freeze_tick(wb_frame_freeze *fz, uint8_t *frame, int w, int h) {
    if (!fz || !fz->is_frozen) return 0;
    if (fz->hold_frames_remaining > 0) {
        fz->hold_frames_remaining--;
        if (frame && fz->frozen_frame)
            memcpy(frame, fz->frozen_frame, w * h * 4);
        return 1;
    }
    fz->is_frozen = 0;
    return 0;
}

void wb_freeze_free(wb_frame_freeze *fz) {
    if (!fz) return;
    free(fz->frozen_frame);
}

/* ================================================================
 * SCREEN SHAKE
 * ================================================================ */

void wb_shake_init(wb_screen_shake *sh) {
    if (!sh) return;
    memset(sh, 0, sizeof(*sh));
    sh->decay = 0.85f;
}

void wb_shake_trigger(wb_screen_shake *sh, float intensity) {
    if (!sh) return;
    sh->active = 1;
    sh->velocity_x = ((float)rand()/RAND_MAX - 0.5f) * intensity * 2;
    sh->velocity_y = ((float)rand()/RAND_MAX - 0.5f) * intensity * 2;
}

void wb_shake_update(wb_screen_shake *sh, float dt) {
    if (!sh || !sh->active) return;
    sh->offset_x += sh->velocity_x * dt * 60;
    sh->offset_y += sh->velocity_y * dt * 60;
    sh->velocity_x *= sh->decay;
    sh->velocity_y *= sh->decay;
    if (fabsf(sh->velocity_x) < 0.01f && fabsf(sh->velocity_y) < 0.01f)
        sh->active = 0;
}

/* ================================================================
 * FLIP / SPIN
 * ================================================================ */

void wb_flip_spin_init(wb_flip_spin *fs) {
    if (!fs) return;
    memset(fs, 0, sizeof(*fs));
    fs->speed = 0.1f;
}

void wb_flip_spin_update(wb_flip_spin *fs, float dt) {
    if (!fs) return;
    fs->angle += fs->speed * dt * 60;
    if (fs->angle > 2*M_PI) fs->angle -= 2*M_PI;
    fs->flip_x = ((int)(fs->angle / M_PI) % 2) == 1;
}

/* ================================================================
 * RECURSION POOP (DROSTE)
 * ================================================================ */

void wb_recursion_init(wb_recursion_poop *rp, float scale, int max_levels) {
    if (!rp) return;
    memset(rp, 0, sizeof(*rp));
    rp->scale = scale > 0.1f ? scale : 0.5f;
    rp->max_levels = max_levels > 0 ? max_levels : 3;
}

void wb_recursion_apply(uint8_t *frame, int w, int h, int levels) {
    if (!frame || levels <= 0) return;
    uint8_t *tmp = (uint8_t *)malloc(w * h * 4);
    if (!tmp) return;
    memcpy(tmp, frame, w * h * 4);
    for (int level = 1; level <= levels; level++) {
        float sc = powf(0.5f, level);
        int sw = (int)(w * sc), sh = (int)(h * sc);
        if (sw < 2 || sh < 2) break;
        int ox = (w-sw)/2, oy = (h-sh)/2;
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                int sx2 = (int)(x/sc), sy2 = (int)(y/sc);
                if (sx2 >= w) sx2 = w-1;
                if (sy2 >= h) sy2 = h-1;
                int dst = ((oy+y)*w+(ox+x))*4, src = (sy2*w+sx2)*4;
                frame[dst]=tmp[src]; frame[dst+1]=tmp[src+1]; frame[dst+2]=tmp[src+2]; frame[dst+3]=255;
            }
    }
    free(tmp);
}

void wb_recursion_free(wb_recursion_poop *rp) {
    if (!rp) return;
    free(rp->inner_buffer);
}

/* ================================================================
 * COMPRESSION TORTURE
 * ================================================================ */

void wb_compression_torture_ytp(uint8_t *frame, int w, int h, int quality) {
    if (!frame) return;
    int bs = 8;
    int quant = (101-quality)*2;
    if (quant < 1) quant = 1;
    for (int by = 0; by < h; by += bs)
        for (int bx = 0; bx < w; bx += bs)
            for (int y = by; y < by+bs && y < h; y++)
                for (int x = bx; x < bx+bs && x < w; x++) {
                    int o = (y*w+x)*4;
                    frame[o]=(frame[o]/quant)*quant;
                    frame[o+1]=(frame[o+1]/quant)*quant;
                    frame[o+2]=(frame[o+2]/quant)*quant;
                }
}

/* ================================================================
 * PHONEME EXTRACTION
 * ================================================================ */

int wb_extract_phonemes(const float *audio, int n_frames, int n_channels,
                         float sample_rate, int *segments, int max_segs) {
    if (!audio || !segments || n_frames <= 0) return 0;
    int n_segs = 0;
    int min_seg = (int)(sample_rate * 0.015f);
    int window = (int)(sample_rate * 0.01f);
    float prev_energy = 0;
    int seg_start = 0;
    for (int i = window; i < n_frames - window; i += window) {
        float energy = 0;
        for (int j = i-window/2; j < i+window/2 && j < n_frames; j++) {
            float s = 0;
            for (int c = 0; c < n_channels; c++)
                s += fabsf(audio[j*n_channels+c]);
            energy += s / n_channels;
        }
        energy /= window;
        if (prev_energy > 0 && n_segs < max_segs) {
            float ratio = energy / (prev_energy+1e-10f);
            if (ratio > 4.0f || ratio < 0.25f) {
                if ((i - seg_start) >= min_seg) {
                    segments[n_segs++] = i;
                    seg_start = i;
                }
            }
        }
        prev_energy = energy;
    }
    return n_segs;
}

/* ================================================================
 * PITCH-TO-NOTE MAPPER
 * ================================================================ */

int wb_pitch_to_note(float freq, int scale_type) {
    if (freq <= 0) return 60;
    int note = (int)(69 + 12*log2f(freq/440.0f) + 0.5f);
    static const int major[] = {0,2,4,5,7,9,11};
    static const int minor[] = {0,2,3,5,7,8,10};
    static const int penta[] = {0,2,4,7,9};
    const int *scale;
    int n;
    switch (scale_type) {
        case 1: scale=minor; n=7; break;
        case 2: scale=penta; n=5; break;
        default: scale=major; n=7; break;
    }
    int octave = note/12, pc = note%12, best=pc, best_d=99;
    for (int i=0;i<n;i++) { int d=abs(pc-scale[i]); if(d>6)d=12-d; if(d<best_d){best_d=d;best=scale[i];} }
    return octave*12+best;
}

/* ================================================================
 * BEAT SEQUENCER
 * ================================================================ */

void wb_beat_seq_init(wb_beat_seq *seq, float bpm) {
    if (!seq) return;
    memset(seq, 0, sizeof(*seq));
    seq->bpm = bpm > 0 ? bpm : 120;
    seq->step_duration = 60.0f/bpm/4;
    for (int t=0;t<8;t++) for (int s=0;s<16;s++) seq->grid[t][s]=-1;
}

void wb_beat_seq_set(wb_beat_seq *seq, int track, int step, int phoneme_idx) {
    if (!seq||track<0||track>=8||step<0||step>=16) return;
    seq->grid[track][step]=phoneme_idx;
}

int wb_beat_seq_tick(wb_beat_seq *seq, float dt) {
    if (!seq||!seq->playing) return -1;
    seq->current_step=(seq->current_step+1)%16;
    for (int t=0;t<8;t++) if (seq->grid[t][seq->current_step]>=0) return seq->grid[t][seq->current_step];
    return -1;
}

/* ================================================================
 * STUTTER BASS
 * ================================================================ */

void wb_stutter_bass_init(wb_stutter_bass *sb) {
    if (!sb) return;
    memset(sb, 0, sizeof(*sb));
    sb->pitch_shift = -12.0f;
}

/* ================================================================
 * MIDI PROBABILITY / RATCHET
 * ================================================================ */

void wb_midi_prob_init(wb_midi_prob *mp) {
    if (!mp) return;
    mp->probability = 1.0f;
    mp->ratchet_count = 1;
    mp->ratchet_div = 1;
}

int wb_midi_prob_fire(wb_midi_prob *mp) {
    if (!mp) return 1;
    return ((float)rand()/RAND_MAX) < mp->probability;
}
