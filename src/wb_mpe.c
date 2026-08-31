/* src/wb_mpe.c — MIDI Polyphonic Expression (MPE) synthesizer.
 * Per-note pitch bend, pressure, and timbre control.
 * Each active note is a saw + lowpass filter voice with independent modulation.
 * Up to 16 simultaneous MPE voices (one per MIDI channel in the zone).
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus.h"

#define MPE_MAX_VOICES 16

/* One MPE voice: saw oscillator + one-pole lowpass filter */
typedef struct mpe_voice {
    int      active;
    int      channel;   /* MIDI channel 0..15 */
    int      note;      /* MIDI note number */
    float    velocity;  /* 0..1 */
    float    phase;     /* 0..1 saw phase */
    float    phase_inc; /* base increment per sample (no bend) */
    float    bend_semitones; /* per-note pitch bend */
    float    pressure;  /* 0..1 amplitude modulation */
    float    timbre;    /* 0..1 filter cutoff modulation */
    float    filter_z1; /* filter state (z1) */
} mpe_voice;

typedef struct wb_mpe {
    uint32_t sr;
    mpe_voice voices[MPE_MAX_VOICES];
    float    global_bend; /* semitones, applied to all voices */
} wb_mpe;

/* ---- helpers ----------------------------------------------------------- */

static float midi_to_freq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

/* One-pole lowpass: cutoff in Hz, returns filtered sample */
static float one_pole_lp(float in, float cutoff_hz, float *z1, float sr) {
    float rc = 1.0f / (2.0f * 3.14159265358979f * cutoff_hz);
    float dt = 1.0f / sr;
    float alpha = dt / (rc + dt);
    *z1 += alpha * (in - *z1);
    return *z1;
}

/* ---- API --------------------------------------------------------------- */

void *wb_mpe_create(uint32_t sr) {
    wb_mpe *mpe = (wb_mpe *)calloc(1, sizeof(wb_mpe));
    if (!mpe) return NULL;
    mpe->sr = sr;
    mpe->global_bend = 0.0f;
    for (int i = 0; i < MPE_MAX_VOICES; i++) {
        mpe->voices[i].active = 0;
        mpe->voices[i].pressure = 1.0f;
        mpe->voices[i].timbre = 0.5f;
    }
    return mpe;
}

void wb_mpe_destroy(void *mpe) {
    free(mpe);
}

void wb_mpe_note_on(void *mpe, int channel, int note, int velocity) {
    wb_mpe *m = (wb_mpe *)mpe;
    if (!m) return;
    /* Find free voice or steal oldest (voice 0 rotation) */
    int slot = -1;
    for (int i = 0; i < MPE_MAX_VOICES; i++) {
        if (!m->voices[i].active) { slot = i; break; }
    }
    if (slot < 0) slot = 0; /* steal voice 0 */

    mpe_voice *v = &m->voices[slot];
    v->active = 1;
    v->channel = channel;
    v->note = note;
    v->velocity = velocity / 127.0f;
    v->phase = 0.0f;
    v->phase_inc = midi_to_freq(note) / m->sr;
    v->bend_semitones = 0.0f;
    v->pressure = 1.0f;
    v->timbre = 0.5f;
    v->filter_z1 = 0.0f;
}

void wb_mpe_note_off(void *mpe, int channel, int note) {
    wb_mpe *m = (wb_mpe *)mpe;
    if (!m) return;
    for (int i = 0; i < MPE_MAX_VOICES; i++) {
        if (m->voices[i].active &&
            m->voices[i].channel == channel &&
            m->voices[i].note == note) {
            m->voices[i].active = 0;
        }
    }
}

void wb_mpe_set_pitch_bend(void *mpe, int channel, int note, float semitones) {
    wb_mpe *m = (wb_mpe *)mpe;
    if (!m) return;
    for (int i = 0; i < MPE_MAX_VOICES; i++) {
        if (m->voices[i].active &&
            m->voices[i].channel == channel &&
            m->voices[i].note == note) {
            m->voices[i].bend_semitones = semitones;
        }
    }
}

void wb_mpe_set_pressure(void *mpe, int channel, int note, float pressure) {
    wb_mpe *m = (wb_mpe *)mpe;
    if (!m) return;
    if (pressure < 0.0f) pressure = 0.0f;
    if (pressure > 1.0f) pressure = 1.0f;
    for (int i = 0; i < MPE_MAX_VOICES; i++) {
        if (m->voices[i].active &&
            m->voices[i].channel == channel &&
            m->voices[i].note == note) {
            m->voices[i].pressure = pressure;
        }
    }
}

void wb_mpe_set_timbre(void *mpe, int channel, int note, float timbre) {
    wb_mpe *m = (wb_mpe *)mpe;
    if (!m) return;
    if (timbre < 0.0f) timbre = 0.0f;
    if (timbre > 1.0f) timbre = 1.0f;
    for (int i = 0; i < MPE_MAX_VOICES; i++) {
        if (m->voices[i].active &&
            m->voices[i].channel == channel &&
            m->voices[i].note == note) {
            m->voices[i].timbre = timbre;
        }
    }
}

int wb_mpe_active_notes(const void *mpe) {
    const wb_mpe *m = (const wb_mpe *)mpe;
    if (!m) return 0;
    int count = 0;
    for (int i = 0; i < MPE_MAX_VOICES; i++) {
        if (m->voices[i].active) count++;
    }
    return count;
}

void wb_mpe_render(void *mpe, wb_sample *out, uint32_t frames) {
    wb_mpe *m = (wb_mpe *)mpe;
    if (!m) return;
    memset(out, 0, frames * sizeof(wb_sample));

    for (int i = 0; i < MPE_MAX_VOICES; i++) {
        mpe_voice *v = &m->voices[i];
        if (!v->active) continue;

        /* Combined bend = per-note + global, in semitones */
        float total_bend = v->bend_semitones + m->global_bend;
        float bend_ratio = powf(2.0f, total_bend / 12.0f);

        for (uint32_t s = 0; s < frames; s++) {
            /* Saw oscillator: phase 0..1 -> -1..1 */
            float saw = 2.0f * v->phase - 1.0f;

            /* Advance phase with bend ratio */
            v->phase += v->phase_inc * bend_ratio;
            if (v->phase >= 1.0f) v->phase -= 1.0f;

            /* Timbre: cutoff 200Hz + timbre*8000Hz */
            float cutoff = 200.0f + v->timbre * 8000.0f;
            float filtered = one_pole_lp(saw, cutoff, &v->filter_z1, m->sr);

            /* Pressure = amplitude modulation */
            float amp = v->velocity * v->pressure;
            out[s] += filtered * amp * 0.3f; /* 0.3 headroom for stacking */
        }
    }
}

void wb_mpe_set_global_bend(void *mpe, float semitones) {
    wb_mpe *m = (wb_mpe *)mpe;
    if (!m) return;
    m->global_bend = semitones;
}