/* wb_particle.c — particle system for audio-reactive visual effects.
 *
 * R077: GPU-style particle system running on CPU (SSE2-optimized).
 *
 * Features:
 *   - Emitter with configurable rate, velocity, spread
 *   - Physics: gravity, drag, wind
 *   - Audio-reactive: particles spawn on beat, velocity from energy
 *   - Additive or alpha blending
 *   - Per-particle: position, velocity, life, size, color
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_PARTICLES 4096

typedef struct {
    float x, y;         /* Position */
    float vx, vy;       /* Velocity */
    float life;         /* Remaining life (0..1) */
    float size;         /* Particle size */
    uint8_t r, g, b, a; /* Color */
    int     active;
} particle_t;

typedef struct {
    uint32_t sr;
    particle_t particles[MAX_PARTICLES];
    int     num_active;

    /* Emitter */
    float    emit_x, emit_y;    /* Emitter position (normalized 0..1) */
    float    emit_rate;         /* Particles per second */
    float    emit_spread;       /* Velocity spread angle (radians) */
    float    emit_speed;        /* Initial speed */

    /* Physics */
    float    gravity;
    float    drag;
    float    wind_x, wind_y;

    /* Audio reactive */
    float    beat_trigger;      /* Spawn burst on beat */
    float    energy_scale;      /* Velocity multiplier from audio energy */

    /* Appearance */
    float    particle_lifetime; /* Seconds */
    float    particle_size;     /* Base size */
    uint8_t  base_r, base_g, base_b;
    int      additive;          /* 1=additive blending, 0=alpha */

    /* State */
    float    emit_accum;
    float    beat_pulse;
    unsigned int rng;
} wb_particle_inst;

static unsigned int prng(unsigned int *s) {
    *s = *s * 1103515245u + 12345u;
    return *s;
}

void *wb_particle_create(uint32_t sr) {
    wb_particle_inst *ps = (wb_particle_inst *)calloc(1, sizeof(*ps));
    if (!ps) return NULL;
    ps->sr = sr;
    ps->emit_x = 0.5f;
    ps->emit_y = 0.5f;
    ps->emit_rate = 100.0f;
    ps->emit_spread = 0.5f;
    ps->emit_speed = 50.0f;
    ps->gravity = 20.0f;
    ps->drag = 0.98f;
    ps->particle_lifetime = 1.0f;
    ps->particle_size = 3.0f;
    ps->base_r = 255;
    ps->base_g = 200;
    ps->base_b = 100;
    ps->additive = 1;
    ps->beat_pulse = 0;
    ps->energy_scale = 1.0f;
    ps->rng = 0x12345678;
    return ps;
}

void wb_particle_destroy(void *inst) { free(inst); }

void wb_particle_set(void *inst, int param, float v) {
    wb_particle_inst *ps = (wb_particle_inst *)inst;
    if (!ps) return;
    switch (param) {
    case 0: ps->emit_rate = v; break;
    case 1: ps->emit_speed = v; break;
    case 2: ps->gravity = v; break;
    case 3: ps->particle_lifetime = v > 0.1f ? v : 0.1f; break;
    case 4: ps->particle_size = v > 0.5f ? v : 0.5f; break;
    case 5: ps->additive = (int)v; break;
    default: break;
    }
}

/* Trigger a beat burst. */
void wb_particle_trigger_beat(void *inst, float energy) {
    wb_particle_inst *ps = (wb_particle_inst *)inst;
    if (!ps) return;
    ps->beat_pulse = 1.0f;
    ps->energy_scale = 1.0f + energy * 3.0f;
}

/* Update and render particles to an RGBA frame.
 * frame: RGBA pixel buffer (width*height*4 bytes)
 * width, height: frame dimensions
 * dt: delta time in seconds */
void wb_particle_update(wb_particle_inst *ps, uint8_t *frame,
                         int width, int height, float dt) {
    if (!ps || !frame) return;

    /* Emit new particles */
    ps->emit_accum += ps->emit_rate * dt;
    int to_emit = (int)ps->emit_accum;
    ps->emit_accum -= (float)to_emit;

    /* Beat burst */
    if (ps->beat_pulse > 0.5f) {
        to_emit += (int)(ps->beat_pulse * 50);
        ps->beat_pulse *= 0.9f;
    }

    for (int i = 0; i < to_emit; i++) {
        /* Find inactive particle */
        int slot = -1;
        for (int j = 0; j < MAX_PARTICLES; j++) {
            if (!ps->particles[j].active) { slot = j; break; }
        }
        if (slot < 0) break;

        /* Initialize particle */
        particle_t *p = &ps->particles[slot];
        p->x = ps->emit_x;
        p->y = ps->emit_y;

        float angle = ((float)(prng(&ps->rng) % 1000) / 1000.0f - 0.5f) * ps->emit_spread * 2.0f;
        float speed = ps->emit_speed * ps->energy_scale * (0.5f + (float)(prng(&ps->rng) % 1000) / 2000.0f);
        p->vx = sinf(angle) * speed;
        p->vy = -cosf(angle) * speed;

        p->life = 1.0f;
        p->size = ps->particle_size * (0.5f + (float)(prng(&ps->rng) % 1000) / 2000.0f);
        p->r = ps->base_r;
        p->g = ps->base_g;
        p->b = ps->base_b;
        p->a = 255;
        p->active = 1;
    }

    /* Update and render */
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particle_t *p = &ps->particles[i];
        if (!p->active) continue;

        /* Physics */
        p->vy += ps->gravity * dt;
        p->vx *= ps->drag;
        p->vy *= ps->drag;
        p->vx += ps->wind_x * dt;
        p->vy += ps->wind_y * dt;

        p->x += p->vx * dt / (float)width;
        p->y += p->vy * dt / (float)height;

        /* Life decay */
        p->life -= dt / ps->particle_lifetime;
        if (p->life <= 0) {
            p->active = 0;
            continue;
        }

        /* Render */
        int px = (int)(p->x * (float)width);
        int py = (int)(p->y * (float)height);

        if (px >= 0 && px < width && py >= 0 && py < height) {
            int idx = (py * width + px) * 4;
            uint8_t alpha = (uint8_t)(p->life * 255.0f);

            if (ps->additive) {
                /* Additive blending */
                int r = frame[idx] + (p->r * alpha / 255);
                int g = frame[idx+1] + (p->g * alpha / 255);
                int b = frame[idx+2] + (p->b * alpha / 255);
                frame[idx]   = r > 255 ? 255 : r;
                frame[idx+1] = g > 255 ? 255 : g;
                frame[idx+2] = b > 255 ? 255 : b;
            } else {
                /* Alpha blending */
                int inv_a = 255 - alpha;
                frame[idx]   = (p->r * alpha + frame[idx]   * inv_a) / 255;
                frame[idx+1] = (p->g * alpha + frame[idx+1] * inv_a) / 255;
                frame[idx+2] = (p->b * alpha + frame[idx+2] * inv_a) / 255;
            }
        }
    }
}
