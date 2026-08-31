/* wb_particle_gpu.c — GPU-accelerated particle system (SIMD/SSE2 CPU fallback).
 *
 * Real-time particle effects for motion graphics (Red Giant/After Effects style).
 * 3D particles with SSE2 vectorized update, 3D→2D projection, additive/alpha blend.
 *
 * Architecture:
 *   - Structure-of-Arrays (SoA) layout for SIMD: 4 particles per SSE2 operation
 *   - Max 10000 particles, each: position(3), velocity(3), life(1), color(4) = 11 floats
 *   - Aligned arrays for _mm_load_ps / _mm_store_ps
 *
 * Pure C11, SSE2 intrinsics. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <emmintrin.h>
#include "wbus.h"

#define MAX_PARTICLES_GPU 10000
#define PARTICLE_STRIDE 16 /* 16 floats per particle slot (aligned for SSE2) */
/* Layout per slot: [x,y,z, pad, vx,vy,vz, pad, life, max_life, size, pad, r,g,b,a] */

typedef struct {
    /* SoA SIMD arrays (16-byte aligned) */
    float *pos_x;     /* [MAX] */
    float *pos_y;     /* [MAX] */
    float *pos_z;     /* [MAX] */
    float *vel_x;     /* [MAX] */
    float *vel_y;     /* [MAX] */
    float *vel_z;     /* [MAX] */
    float *life;      /* [MAX]  remaining life in seconds */
    float *max_life;  /* [MAX]  total life (for color interpolation) */
    float *size;      /* [MAX]  particle size in pixels */
    float *color_r;   /* [MAX]  0..1 */
    float *color_g;   /* [MAX]  0..1 */
    float *color_b;   /* [MAX]  0..1 */
    float *color_a;   /* [MAX]  0..1 */
    uint8_t *active;  /* [MAX]  1=alive, 0=dead */

    uint32_t max_count;
    uint32_t active_count;

    /* Physics */
    float gravity[3];      /* gx, gy, gz */

    /* Emission parameters */
    float lifetime_min;
    float lifetime_max;
    float size_min;
    float size_max;
    uint32_t start_color;  /* ARGB */
    uint32_t end_color;    /* ARGB */

    /* Blend mode: 0=alpha, 1=additive */
    int blend_additive;

    /* RNG state */
    unsigned int rng;

    /* Projection parameters */
    float fov;             /* field of view scale */
    float cam_z;           /* camera distance */
} wb_particle_gpu_inst;

/* ---- helpers ---- */

static unsigned int prng_next(unsigned int *s) {
    *s = *s * 1103515245u + 12345u;
    return *s;
}

static float prng_float(unsigned int *s) {
    return (float)(prng_next(s) % 10000) / 10000.0f;
}

/* Extract ARGB components (0..1) from a uint32_t ARGB color. */
static void unpack_argb(uint32_t c, float *r, float *g, float *b, float *a) {
    *a = (float)((c >> 24) & 0xFF) / 255.0f;
    *r = (float)((c >> 16) & 0xFF) / 255.0f;
    *g = (float)((c >> 8) & 0xFF) / 255.0f;
    *b = (float)(c & 0xFF) / 255.0f;
}

/* Aligned alloc / free */
static float *aligned_alloc_f32(uint32_t n) {
    void *p = NULL;
    if (posix_memalign(&p, 16, n * sizeof(float)) != 0) return NULL;
    memset(p, 0, n * sizeof(float));
    return (float *)p;
}

static uint8_t *aligned_alloc_u8(uint32_t n) {
    void *p = calloc(n, 1);
    return (uint8_t *)p;
}

/* ---- API ---- */

void *wb_gpu_particle_create(uint32_t max_particles) {
    if (max_particles == 0 || max_particles > MAX_PARTICLES_GPU)
        max_particles = MAX_PARTICLES_GPU;

    wb_particle_gpu_inst *ps = (wb_particle_gpu_inst *)calloc(1, sizeof(*ps));
    if (!ps) return NULL;

    ps->max_count = max_particles;
    ps->active_count = 0;
    ps->rng = 0xDEADBEEF;

    ps->pos_x    = aligned_alloc_f32(max_particles);
    ps->pos_y    = aligned_alloc_f32(max_particles);
    ps->pos_z    = aligned_alloc_f32(max_particles);
    ps->vel_x    = aligned_alloc_f32(max_particles);
    ps->vel_y    = aligned_alloc_f32(max_particles);
    ps->vel_z    = aligned_alloc_f32(max_particles);
    ps->life     = aligned_alloc_f32(max_particles);
    ps->max_life = aligned_alloc_f32(max_particles);
    ps->size     = aligned_alloc_f32(max_particles);
    ps->color_r  = aligned_alloc_f32(max_particles);
    ps->color_g  = aligned_alloc_f32(max_particles);
    ps->color_b  = aligned_alloc_f32(max_particles);
    ps->color_a  = aligned_alloc_f32(max_particles);
    ps->active   = aligned_alloc_u8(max_particles);

    if (!ps->pos_x || !ps->pos_y || !ps->pos_z ||
        !ps->vel_x || !ps->vel_y || !ps->vel_z ||
        !ps->life || !ps->max_life || !ps->size ||
        !ps->color_r || !ps->color_g || !ps->color_b || !ps->color_a ||
        !ps->active) {
        wb_gpu_particle_destroy(ps);
        return NULL;
    }

    /* Defaults */
    ps->gravity[0] = 0.0f;
    ps->gravity[1] = -9.8f;
    ps->gravity[2] = 0.0f;
    ps->lifetime_min = 1.0f;
    ps->lifetime_max = 3.0f;
    ps->size_min = 2.0f;
    ps->size_max = 6.0f;
    ps->start_color = 0xFFFFFFFF; /* white */
    ps->end_color   = 0x00FF8800; /* transparent orange */
    ps->blend_additive = 1;
    ps->fov = 500.0f;
    ps->cam_z = 10.0f;

    return ps;
}

void wb_gpu_particle_destroy(void *inst) {
    if (!inst) return;
    wb_particle_gpu_inst *ps = (wb_particle_gpu_inst *)inst;
    free(ps->pos_x); free(ps->pos_y); free(ps->pos_z);
    free(ps->vel_x); free(ps->vel_y); free(ps->vel_z);
    free(ps->life); free(ps->max_life); free(ps->size);
    free(ps->color_r); free(ps->color_g); free(ps->color_b); free(ps->color_a);
    free(ps->active);
    free(ps);
}

void wb_gpu_particle_set_gravity(void *inst, float gx, float gy, float gz) {
    wb_particle_gpu_inst *ps = (wb_particle_gpu_inst *)inst;
    if (!ps) return;
    ps->gravity[0] = gx;
    ps->gravity[1] = gy;
    ps->gravity[2] = gz;
}

void wb_gpu_particle_set_lifetime(void *inst, float min_sec, float max_sec) {
    wb_particle_gpu_inst *ps = (wb_particle_gpu_inst *)inst;
    if (!ps) return;
    if (min_sec < 0.01f) min_sec = 0.01f;
    if (max_sec < min_sec) max_sec = min_sec;
    ps->lifetime_min = min_sec;
    ps->lifetime_max = max_sec;
}

void wb_gpu_particle_set_colors(void *inst, uint32_t start_color, uint32_t end_color) {
    wb_particle_gpu_inst *ps = (wb_particle_gpu_inst *)inst;
    if (!ps) return;
    ps->start_color = start_color;
    ps->end_color = end_color;
}

void wb_gpu_particle_set_size(void *inst, float min_size, float max_size) {
    wb_particle_gpu_inst *ps = (wb_particle_gpu_inst *)inst;
    if (!ps) return;
    if (min_size < 0.5f) min_size = 0.5f;
    if (max_size < min_size) max_size = min_size;
    ps->size_min = min_size;
    ps->size_max = max_size;
}

int wb_gpu_particle_get_active_count(const void *inst) {
    const wb_particle_gpu_inst *ps = (const wb_particle_gpu_inst *)inst;
    if (!ps) return 0;
    return (int)ps->active_count;
}

/* ---- Emit ---- */

void wb_gpu_particle_emit(void *inst, float x, float y, float z, int count) {
    wb_particle_gpu_inst *ps = (wb_particle_gpu_inst *)inst;
    if (!ps || count <= 0) return;

    float sr, sg, sb, sa;
    float er, eg, eb, ea;
    unpack_argb(ps->start_color, &sr, &sg, &sb, &sa);
    unpack_argb(ps->end_color, &er, &eg, &eb, &ea);

    for (int i = 0; i < count; i++) {
        /* Find a dead slot */
        uint32_t slot = 0xFFFFFFFF;
        for (uint32_t j = 0; j < ps->max_count; j++) {
            if (!ps->active[j]) { slot = j; break; }
        }
        if (slot == 0xFFFFFFFF) break; /* pool full */

        /* Random velocity spread */
        float angle1 = prng_float(&ps->rng) * 6.283185307f;
        float angle2 = (prng_float(&ps->rng) - 0.5f) * 3.14159265f;
        float speed = 2.0f + prng_float(&ps->rng) * 5.0f;

        ps->pos_x[slot] = x;
        ps->pos_y[slot] = y;
        ps->pos_z[slot] = z;

        ps->vel_x[slot] = cosf(angle1) * sinf(angle2) * speed;
        ps->vel_y[slot] = cosf(angle2) * speed;
        ps->vel_z[slot] = sinf(angle1) * sinf(angle2) * speed;

        float life = ps->lifetime_min + prng_float(&ps->rng) * (ps->lifetime_max - ps->lifetime_min);
        ps->life[slot] = life;
        ps->max_life[slot] = life;

        ps->size[slot] = ps->size_min + prng_float(&ps->rng) * (ps->size_max - ps->size_min);

        /* Interpolate start→end color by random t for variety */
        float t = prng_float(&ps->rng);
        ps->color_r[slot] = sr + (er - sr) * t;
        ps->color_g[slot] = sg + (eg - sg) * t;
        ps->color_b[slot] = sb + (eb - sb) * t;
        ps->color_a[slot] = sa + (ea - sa) * t;

        ps->active[slot] = 1;
        ps->active_count++;
    }
}

/* ---- Update (SSE2 vectorized, 4 particles at a time) ---- */

void wb_gpu_particle_update(void *inst, float dt) {
    wb_particle_gpu_inst *ps = (wb_particle_gpu_inst *)inst;
    if (!ps || dt <= 0.0f) return;

    uint32_t n = ps->max_count;
    float gx = ps->gravity[0];
    float gy = ps->gravity[1];
    float gz = ps->gravity[2];

    /* SSE2 constants */
    __m128 vdt = _mm_set1_ps(dt);
    __m128 vgx = _mm_set1_ps(gx);
    __m128 vgy = _mm_set1_ps(gy);
    __m128 vgz = _mm_set1_ps(gz);
    __m128 zero = _mm_setzero_ps();

    /* Process 4 particles at a time (SSE2 = 4 floats) */
    uint32_t simd_end = n & ~3u; /* round down to multiple of 4 */

    for (uint32_t i = 0; i < simd_end; i += 4) {
        /* Check if any of the 4 are active; skip if all dead */
        uint8_t a0 = ps->active[i], a1 = ps->active[i+1];
        uint8_t a2 = ps->active[i+2], a3 = ps->active[i+3];
        if (!a0 && !a1 && !a2 && !a3) continue;

        /* Load positions */
        __m128 px = _mm_load_ps(&ps->pos_x[i]);
        __m128 py = _mm_load_ps(&ps->pos_y[i]);
        __m128 pz = _mm_load_ps(&ps->pos_z[i]);

        /* Load velocities */
        __m128 vx = _mm_load_ps(&ps->vel_x[i]);
        __m128 vy = _mm_load_ps(&ps->vel_y[i]);
        __m128 vz = _mm_load_ps(&ps->vel_z[i]);

        /* Load life */
        __m128 life = _mm_load_ps(&ps->life[i]);

        /* Apply gravity to velocity: v += g * dt */
        vx = _mm_add_ps(vx, _mm_mul_ps(vgx, vdt));
        vy = _mm_add_ps(vy, _mm_mul_ps(vgy, vdt));
        vz = _mm_add_ps(vz, _mm_mul_ps(vgz, vdt));

        /* Integrate position: p += v * dt */
        px = _mm_add_ps(px, _mm_mul_ps(vx, vdt));
        py = _mm_add_ps(py, _mm_mul_ps(vy, vdt));
        pz = _mm_add_ps(pz, _mm_mul_ps(vz, vdt));

        /* Decrement life */
        life = _mm_sub_ps(life, vdt);

        /* Store back */
        _mm_store_ps(&ps->pos_x[i], px);
        _mm_store_ps(&ps->pos_y[i], py);
        _mm_store_ps(&ps->pos_z[i], pz);
        _mm_store_ps(&ps->vel_x[i], vx);
        _mm_store_ps(&ps->vel_y[i], vy);
        _mm_store_ps(&ps->vel_z[i], vz);
        _mm_store_ps(&ps->life[i], life);

        /* Kill dead particles (life <= 0) — only decrement active_count
         * for particles that WERE active before this update */
        __m128 mask = _mm_cmpgt_ps(life, zero);
        int m = _mm_movemask_ps(mask);

        if (a0 && !(m & 1)) { ps->active[i] = 0; ps->active_count--; }
        if (a1 && !(m & 2)) { ps->active[i+1] = 0; ps->active_count--; }
        if (a2 && !(m & 4)) { ps->active[i+2] = 0; ps->active_count--; }
        if (a3 && !(m & 8)) { ps->active[i+3] = 0; ps->active_count--; }
    }

    /* Handle remainder (n not multiple of 4) */
    for (uint32_t i = simd_end; i < n; i++) {
        if (!ps->active[i]) continue;

        ps->vel_x[i] += gx * dt;
        ps->vel_y[i] += gy * dt;
        ps->vel_z[i] += gz * dt;

        ps->pos_x[i] += ps->vel_x[i] * dt;
        ps->pos_y[i] += ps->vel_y[i] * dt;
        ps->pos_z[i] += ps->vel_z[i] * dt;

        ps->life[i] -= dt;
        if (ps->life[i] <= 0.0f) {
            ps->active[i] = 0;
            ps->active_count--;
        }
    }
}

/* ---- Render: project 3D→2D, draw as colored points in RGBA buffer ---- */

void wb_gpu_particle_render(void *inst, uint8_t *rgba_out, int w, int h) {
    wb_particle_gpu_inst *ps = (wb_particle_gpu_inst *)inst;
    if (!ps || !rgba_out || w <= 0 || h <= 0) return;

    float fov = ps->fov;
    float cam_z = ps->cam_z;
    float cx = (float)w * 0.5f;
    float cy = (float)h * 0.5f;

    for (uint32_t i = 0; i < ps->max_count; i++) {
        if (!ps->active[i]) continue;

        float z = ps->pos_z[i] + cam_z;
        if (z < 0.1f) z = 0.1f; /* avoid div by zero */

        /* Perspective projection */
        float inv_z = fov / z;
        int sx = (int)(cx + ps->pos_x[i] * inv_z);
        int sy = (int)(cy - ps->pos_y[i] * inv_z); /* flip y for screen */

        if (sx < 0 || sx >= w || sy < 0 || sy >= h) continue;

        /* Life fraction for alpha fade */
        float life_frac = 0.0f;
        if (ps->max_life[i] > 0.0f)
            life_frac = ps->life[i] / ps->max_life[i];
        if (life_frac < 0.0f) life_frac = 0.0f;
        if (life_frac > 1.0f) life_frac = 1.0f;

        /* Color: interpolate start→end by (1 - life_frac) */
        float sr, sg, sb, sa, er, eg, eb, ea;
        unpack_argb(ps->start_color, &sr, &sg, &sb, &sa);
        unpack_argb(ps->end_color, &er, &eg, &eb, &ea);

        float t = 1.0f - life_frac;
        float r = sr + (er - sr) * t;
        float g = sg + (eg - sg) * t;
        float b = sb + (eb - sb) * t;
        float a = sa + (ea - sa) * t;

        /* Apply life-based alpha fade */
        a *= life_frac;

        uint8_t pr = (uint8_t)(r * 255.0f);
        uint8_t pg = (uint8_t)(g * 255.0f);
        uint8_t pb = (uint8_t)(b * 255.0f);
        uint8_t pa = (uint8_t)(a * 255.0f);

        if (pa == 0) continue;

        /* Draw particle as a small square (size based on particle size and distance) */
        float size = ps->size[i] * (fov / z);
        if (size < 1.0f) size = 1.0f;
        int radius = (int)(size * 0.5f);
        if (radius < 0) radius = 0;

        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                int px = sx + dx;
                int py = sy + dy;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;

                int idx = (py * w + px) * 4;

                if (ps->blend_additive) {
                    /* Additive blending */
                    int rr = rgba_out[idx]   + (pr * pa / 255);
                    int gg = rgba_out[idx+1] + (pg * pa / 255);
                    int bb = rgba_out[idx+2] + (pb * pa / 255);
                    rgba_out[idx]   = rr > 255 ? 255 : (uint8_t)rr;
                    rgba_out[idx+1] = gg > 255 ? 255 : (uint8_t)gg;
                    rgba_out[idx+2] = bb > 255 ? 255 : (uint8_t)bb;
                } else {
                    /* Alpha blending */
                    int inv_a = 255 - pa;
                    rgba_out[idx]   = (uint8_t)((pr * pa + rgba_out[idx]   * inv_a) / 255);
                    rgba_out[idx+1] = (uint8_t)((pg * pa + rgba_out[idx+1] * inv_a) / 255);
                    rgba_out[idx+2] = (uint8_t)((pb * pa + rgba_out[idx+2] * inv_a) / 255);
                }
            }
        }
    }
}