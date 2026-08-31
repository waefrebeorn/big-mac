/* wb_drum_rack.c — pad-based drum sampler (Ableton Drum Rack style).
 *
 * 64 pads (8x8 grid). Each pad is a one-shot sample player with volume, pan,
 * mute, solo. Up to 32 simultaneous voices (round-robin voice allocation).
 * Linear interpolation for sample playback. Velocity-sensitive volume.
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "wbus.h"
#include "wb_internal.h"

#define DRUM_RACK_PAD_COUNT    64
#define DRUM_RACK_MAX_VOICES   32

/* One pad: owns its sample buffer + per-pad mix state. */
typedef struct {
    wb_sample *data;        /* interleaved sample data (owned) */
    uint32_t   frames;
    uint32_t   channels;
    int        loaded;      /* 1 if a sample is loaded on this pad */
    float      volume;      /* linear gain 0..1+ */
    float      pan;         /* -1..1 */
    int        mute;
    int        solo;
} wb_drum_pad_t;

/* One playing voice (references a pad, walks through its sample). */
typedef struct {
    int        active;
    int        pad_index;   /* which pad this voice belongs to */
    double     pos;         /* fractional sample position */
    float      velocity;    /* 0..1 */
} wb_drum_voice_t;

/* The drum rack instance. */
typedef struct wb_drum_rack {
    uint32_t         sr;
    wb_drum_pad_t    pads[DRUM_RACK_PAD_COUNT];
    wb_drum_voice_t  voices[DRUM_RACK_MAX_VOICES];
    int              voice_rr;       /* round-robin next voice index */
    float            master_volume;
    int              any_solo;       /* recomputed on process */
} wb_drum_rack;

/* ---- helpers ------------------------------------------------------------ */

static int pad_valid(int pad_index) {
    return pad_index >= 0 && pad_index < DRUM_RACK_PAD_COUNT;
}

/* Recompute the any_solo flag from pad states. */
static void recompute_solo(wb_drum_rack *r) {
    r->any_solo = 0;
    for (int i = 0; i < DRUM_RACK_PAD_COUNT; i++) {
        if (r->pads[i].solo) { r->any_solo = 1; break; }
    }
}

/* ---- API --------------------------------------------------------------- */

wb_drum_rack *wb_drum_rack_create(uint32_t sr) {
    wb_drum_rack *r = (wb_drum_rack *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->sr = sr;
    r->master_volume = 1.0f;
    r->voice_rr = 0;
    /* Default pad volumes to 1.0, pan to 0 (center). */
    for (int i = 0; i < DRUM_RACK_PAD_COUNT; i++) {
        r->pads[i].volume = 1.0f;
        r->pads[i].pan = 0.0f;
    }
    return r;
}

void wb_drum_rack_destroy(wb_drum_rack *r) {
    if (!r) return;
    for (int i = 0; i < DRUM_RACK_PAD_COUNT; i++) {
        free(r->pads[i].data);
    }
    free(r);
}

int wb_drum_rack_load_pad(wb_drum_rack *r, int pad_index,
                          const wb_sample *audio, uint32_t frames,
                          uint32_t channels) {
    if (!r || !pad_valid(pad_index) || !audio || frames == 0 || channels == 0)
        return -1;
    wb_drum_pad_t *p = &r->pads[pad_index];
    free(p->data);
    size_t n = (size_t)frames * channels;
    p->data = (wb_sample *)malloc(n * sizeof(wb_sample));
    if (!p->data) { p->loaded = 0; return -1; }
    memcpy(p->data, audio, n * sizeof(wb_sample));
    p->frames = frames;
    p->channels = channels;
    p->loaded = 1;
    return 0;
}

int wb_drum_rack_load_pad_file(wb_drum_rack *r, int pad_index,
                               const char *path) {
    if (!r || !path || !pad_valid(pad_index)) return -1;
    float *data = NULL;
    uint32_t frames = 0;
    int channels = 0, sr = 0;
    if (wb_wav_read_pcm16(path, &data, &frames, &channels, &sr) != 0)
        return -1;
    int rc = wb_drum_rack_load_pad(r, pad_index, data, frames, (uint32_t)channels);
    free(data);
    return rc;
}

int wb_drum_rack_trigger(wb_drum_rack *r, int pad_index, float velocity) {
    if (!r || !pad_valid(pad_index)) return -1;
    wb_drum_pad_t *p = &r->pads[pad_index];
    if (!p->loaded) return -1;

    /* Solo gate: if any pad is soloed and this one isn't, skip. */
    if (r->any_solo && !p->solo) return -1;
    if (p->mute) return -1;

    /* Round-robin voice allocation. */
    int slot = -1;
    for (int i = 0; i < DRUM_RACK_MAX_VOICES; i++) {
        int idx = (r->voice_rr + i) % DRUM_RACK_MAX_VOICES;
        if (!r->voices[idx].active) { slot = idx; break; }
    }
    if (slot < 0) {
        /* All voices busy — steal the next round-robin slot. */
        slot = r->voice_rr % DRUM_RACK_MAX_VOICES;
    }
    r->voice_rr = (slot + 1) % DRUM_RACK_MAX_VOICES;

    wb_drum_voice_t *v = &r->voices[slot];
    v->active = 1;
    v->pad_index = pad_index;
    v->pos = 0.0;
    v->velocity = velocity > 1.0f ? 1.0f : (velocity < 0.0f ? 0.0f : velocity);
    return 0;
}

int wb_drum_rack_trigger_note(wb_drum_rack *r, int midi_note, float velocity) {
    int pad_index = midi_note - 36;  /* MIDI note 36 = pad 0 */
    return wb_drum_rack_trigger(r, pad_index, velocity);
}

void wb_drum_rack_process(wb_drum_rack *r, wb_sample *out, uint32_t frames) {
    if (!r || !out) return;
    recompute_solo(r);

    for (uint32_t i = 0; i < frames; i++) {
        float mix_l = 0.0f, mix_r = 0.0f;
        for (int v = 0; v < DRUM_RACK_MAX_VOICES; v++) {
            wb_drum_voice_t *vv = &r->voices[v];
            if (!vv->active) continue;
            wb_drum_pad_t *p = &r->pads[vv->pad_index];
            if (!p->loaded) { vv->active = 0; continue; }

            uint32_t idx = (uint32_t)vv->pos;
            if (idx >= p->frames) {
                vv->active = 0;
                continue;
            }

            /* Linear interpolation. */
            float frac = (float)(vv->pos - (double)idx);
            uint32_t i0 = idx;
            uint32_t i1 = (idx + 1 < p->frames) ? idx + 1 : idx;

            float gain = vv->velocity * p->volume * r->master_volume;

            /* Pan: -1 = full left, +1 = full right. */
            float pan = p->pan;
            float left_gain  = gain * (pan <= 0.0f ? 1.0f : 1.0f - pan);
            float right_gain = gain * (pan >= 0.0f ? 1.0f : 1.0f + pan);

            if (p->channels >= 2) {
                float s0_l = p->data[i0 * p->channels];
                float s1_l = p->data[i1 * p->channels];
                float s0_r = p->data[i0 * p->channels + 1];
                float s1_r = p->data[i1 * p->channels + 1];
                float s_l = s0_l + (s1_l - s0_l) * frac;
                float s_r = s0_r + (s1_r - s0_r) * frac;
                mix_l += s_l * left_gain;
                mix_r += s_r * right_gain;
            } else {
                /* Mono sample — apply pan. */
                float s0 = p->data[i0];
                float s1 = p->data[i1];
                float s = s0 + (s1 - s0) * frac;
                mix_l += s * left_gain;
                mix_r += s * right_gain;
            }

            vv->pos += 1.0;
        }
        out[i * 2]     = mix_l;
        out[i * 2 + 1] = mix_r;
    }
}

void wb_drum_rack_set_pad_volume(wb_drum_rack *r, int pad, float vol) {
    if (!r || !pad_valid(pad)) return;
    r->pads[pad].volume = vol;
}

void wb_drum_rack_set_pad_pan(wb_drum_rack *r, int pad, float pan) {
    if (!r || !pad_valid(pad)) return;
    r->pads[pad].pan = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
}

void wb_drum_rack_set_pad_mute(wb_drum_rack *r, int pad, int mute) {
    if (!r || !pad_valid(pad)) return;
    r->pads[pad].mute = mute ? 1 : 0;
}

void wb_drum_rack_set_pad_solo(wb_drum_rack *r, int pad, int solo) {
    if (!r || !pad_valid(pad)) return;
    r->pads[pad].solo = solo ? 1 : 0;
    recompute_solo(r);
}

void wb_drum_rack_set_master_volume(wb_drum_rack *r, float vol) {
    if (!r) return;
    r->master_volume = vol;
}

void wb_drum_rack_clear(wb_drum_rack *r) {
    if (!r) return;
    for (int i = 0; i < DRUM_RACK_MAX_VOICES; i++) {
        r->voices[i].active = 0;
    }
}

int wb_drum_rack_pad_count(wb_drum_rack *r) {
    if (!r) return 0;
    int count = 0;
    for (int i = 0; i < DRUM_RACK_PAD_COUNT; i++) {
        if (r->pads[i].loaded) count++;
    }
    return count;
}