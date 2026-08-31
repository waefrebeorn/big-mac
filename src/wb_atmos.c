/* wb_atmos.c — Dolby Atmos-style object-based spatial audio panner.
 *
 * Object-based binaural renderer: up to 16 mono audio objects positioned
 * in 3D space (azimuth, elevation, distance) are rendered to a stereo
 * binaural output using a parametric HRTF model:
 *
 *   - ITD: Woodsworth formula (interaural time delay from azimuth)
 *   - IID: head-shadow level difference (lowpass on far ear)
 *   - Distance: inverse-square law + air-absorption lowpass
 *   - Elevation: pinna notch filter (simplified)
 *
 * Pure C11, zero third-party. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WB_ATMOS_MAX_OBJECTS 16
#define ATMOS_HEAD_RADIUS   0.0875f   /* meters (mean human head) */
#define ATMOS_SOUND_SPEED   343.0f    /* m/s */
#define ATMOS_MAX_ITD_SAMPLES 64      /* enough for ~0.6ms at 44.1kHz */

/* Per-object state */
typedef struct {
    float azimuth;     /* -180..180 deg, 0 = front */
    float elevation;   /* -90..90 deg, 0 = horizontal */
    float distance;    /* 0.1..10 m */
    float gain;        /* linear multiplier */

    /* Derived filter/spatial state */
    float itd_samples;    /* fractional delay for far ear */
    float far_gain;       /* head-shadow attenuation for far ear */
    float near_gain;      /* near-ear gain (slight boost) */

    /* Far-ear lowpass (head shadowing): first-order IIR */
    float lp_state;
    float lp_coeff;

    /* Pinna notch (elevation): simple 2nd-order state */
    float notch_b0, notch_b1, notch_b2, notch_a1, notch_a2;
    float notch_s1, notch_s2;

    /* ITD delay line for far ear */
    wb_sample *delay_buf;
    uint32_t   delay_len;
    uint32_t   delay_pos;
} wb_atmos_object;

struct wb_atmos {
    uint32_t    sr;
    uint32_t    count;       /* number of active objects */
    wb_atmos_object objects[WB_ATMOS_MAX_OBJECTS];
};

/* ---- helpers ---- */

static float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* Compute Woodsworth ITD (seconds) from azimuth in degrees.
 * Woodsworth 1960 approximation:
 *   ITD = (r/c) * (sin(theta) + theta)   [theta in radians, 0..pi]
 * where r = head radius, c = sound speed.
 * For left/right asymmetry we use the azimuth directly. */
static float woodsworth_itd(float azimuth_deg) {
    float theta = azimuth_deg * (float)M_PI / 180.0f;
    float r = ATMOS_HEAD_RADIUS;
    float c = ATMOS_SOUND_SPEED;
    float itd = (r / c) * (sinf(theta) + theta);
    return itd; /* signed: positive = sound arrives at right ear first */
}

/* Head-shadow IID: far ear is attenuated. Approximate as a smooth
 * function of azimuth. At +/-90 deg the far ear is ~6-10 dB down. */
static float head_shadow_gain(float azimuth_deg) {
    float a = clampf(azimuth_deg, -180.0f, 180.0f);
    float abs_a = fabsf(a);
    /* Near ear: slight boost toward the source direction */
    /* Far ear: attenuated when source is on the opposite side */
    float shadow;
    if (abs_a <= 90.0f) {
        /* Source is in front hemisphere */
        shadow = 1.0f - 0.005f * abs_a; /* gentle rolloff */
    } else {
        /* Source is in back hemisphere — more shadowing */
        shadow = 0.55f - 0.002f * (180.0f - abs_a);
    }
    return clampf(shadow, 0.1f, 1.0f);
}

/* Pinna elevation filter coefficients (simplified).
 * Elevation changes the spectral tilt. Above the ear (elev > 0)
 * boosts highs; below cuts highs. Models the pinna's direction-dependent
 * filtering without the complexity of a true notch filter. */
static void compute_pinna_filter(float elevation_deg, float coeffs[5]) {
    float elev = clampf(elevation_deg, -90.0f, 90.0f);

    /* First-order tilt filter: positive elevation boosts highs. */
    float tilt = elev / 90.0f;       /* -1..1 */
    float alpha = 0.5f + tilt * 0.3f; /* 0.2..0.8 */
    coeffs[0] = alpha;               /* b0 */
    coeffs[1] = -(1.0f - alpha);     /* b1 (feedback, used as state) */
    coeffs[2] = 0.0f;                /* b2 */
    coeffs[3] = alpha - 1.0f;        /* a1 */
    coeffs[4] = 0.0f;                /* a2 */
}

/* ---- public API ---- */

void *wb_atmos_create(uint32_t sr) {
    struct wb_atmos *a = (struct wb_atmos *)calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->sr = sr;
    a->count = WB_ATMOS_MAX_OBJECTS;

    /* Allocate delay lines for ITD */
    uint32_t max_delay = ATMOS_MAX_ITD_SAMPLES + 1;
    for (int i = 0; i < WB_ATMOS_MAX_OBJECTS; i++) {
        a->objects[i].delay_buf = (wb_sample *)calloc((size_t)max_delay, sizeof(wb_sample));
        a->objects[i].delay_len = max_delay;
        a->objects[i].delay_pos = 0;
        a->objects[i].distance = 1.0f;
        a->objects[i].gain = 1.0f;
        a->objects[i].near_gain = 1.0f;
        a->objects[i].far_gain = 1.0f;
    }
    return a;
}

void wb_atmos_destroy(void *inst) {
    struct wb_atmos *a = (struct wb_atmos *)inst;
    if (!a) return;
    for (int i = 0; i < WB_ATMOS_MAX_OBJECTS; i++) {
        free(a->objects[i].delay_buf);
    }
    free(a);
}

void wb_atmos_set_position(void *inst, int obj_id, float azimuth, float elevation, float distance) {
    struct wb_atmos *a = (struct wb_atmos *)inst;
    if (!a || obj_id < 0 || obj_id >= WB_ATMOS_MAX_OBJECTS) return;

    wb_atmos_object *obj = &a->objects[obj_id];
    obj->azimuth = clampf(azimuth, -180.0f, 180.0f);
    obj->elevation = clampf(elevation, -90.0f, 90.0f);
    obj->distance = clampf(distance, 0.1f, 10.0f);

    /* Compute ITD from azimuth */
    float itd_sec = woodsworth_itd(obj->azimuth);
    obj->itd_samples = fabsf(itd_sec) * (float)a->sr;

    /* Compute IID */
    if (obj->azimuth >= 0.0f) {
        /* Source on right: left ear is far */
        obj->far_gain = head_shadow_gain(obj->azimuth);
        obj->near_gain = 1.0f + (1.0f - head_shadow_gain(obj->azimuth)) * 0.3f;
    } else {
        /* Source on left: right ear is far */
        obj->far_gain = head_shadow_gain(obj->azimuth);
        obj->near_gain = 1.0f + (1.0f - head_shadow_gain(obj->azimuth)) * 0.3f;
    }

    /* Distance: inverse-square law */
    float dist_gain = 1.0f / (obj->distance * obj->distance);
    /* Normalize so 1m = unity */
    dist_gain *= 1.0f; /* 1/(1*1) = 1 at 1m */
    obj->near_gain *= dist_gain;
    obj->far_gain *= dist_gain;

    /* Air absorption: distance-dependent lowpass coefficient.
     * More distance = more high-freq loss. fc ~ 18kHz at 1m, ~ 5kHz at 10m. */
    float air_fc = 18000.0f / obj->distance;
    if (air_fc > 20000.0f) air_fc = 20000.0f;
    if (air_fc < 2000.0f) air_fc = 2000.0f;
    /* First-order LP coefficient: coeff = 1 - exp(-2*pi*fc/sr) */
    float omega = 2.0f * (float)M_PI * air_fc / (float)a->sr;
    obj->lp_coeff = 1.0f - expf(-omega);

    /* Pinna elevation filter */
    float nc[5];
    compute_pinna_filter(obj->elevation, nc);
    obj->notch_b0 = nc[0];
    obj->notch_b1 = nc[1];
    obj->notch_b2 = nc[2];
    obj->notch_a1 = nc[3];
    obj->notch_a2 = nc[4];
}

void wb_atmos_set_object_gain(void *inst, int obj_id, float gain) {
    struct wb_atmos *a = (struct wb_atmos *)inst;
    if (!a || obj_id < 0 || obj_id >= WB_ATMOS_MAX_OBJECTS) return;
    a->objects[obj_id].gain = clampf(gain, 0.0f, 10.0f);
}

int wb_atmos_get_object_count(const void *inst) {
    const struct wb_atmos *a = (const struct wb_atmos *)inst;
    if (!a) return 0;
    return (int)a->count;
}

int wb_atmos_process(void *inst, const wb_sample **inputs, wb_sample **output_binaural,
                     int num_objects, uint32_t frames) {
    struct wb_atmos *a = (struct wb_atmos *)inst;
    if (!a || !inputs || !output_binaural || num_objects <= 0) return -1;
    if (num_objects > WB_ATMOS_MAX_OBJECTS) num_objects = WB_ATMOS_MAX_OBJECTS;

    /* Clear output buffers */
    memset(output_binaural[0], 0, frames * sizeof(wb_sample));
    memset(output_binaural[1], 0, frames * sizeof(wb_sample));

    for (int obj = 0; obj < num_objects; obj++) {
        const wb_sample *in = inputs[obj];
        if (!in) continue;

        wb_atmos_object *o = &a->objects[obj];
        float obj_gain = o->gain;
        if (obj_gain <= 0.0f) continue;

        /* Determine which ear is "near" based on azimuth sign */
        int source_on_right = (o->azimuth >= 0.0f) ? 1 : 0;

        float near_gain = o->near_gain * obj_gain;
        float far_gain = o->far_gain * obj_gain;

        /* Process each sample */
        for (uint32_t i = 0; i < frames; i++) {
            float x = in[i];

            /* Apply elevation filter (pinna) */
            float y = o->notch_b0 * x + o->notch_s1;
            o->notch_s1 = o->notch_b1 * x - o->notch_a1 * y;

            /* Apply air-absorption lowpass */
            o->lp_state += o->lp_coeff * (y - o->lp_state);
            float mono = o->lp_state;

            /* Near ear: direct gain */
            float near_sample = mono * near_gain;

            /* Far ear: delayed via ITD delay line + far gain */
            /* Write to delay line */
            o->delay_buf[o->delay_pos] = mono;
            uint32_t delay_samples = (uint32_t)(o->itd_samples + 0.5f);
            if (delay_samples >= o->delay_len - 1)
                delay_samples = o->delay_len - 2;
            uint32_t read_pos = (o->delay_pos + o->delay_len - delay_samples) % o->delay_len;
            float delayed = o->delay_buf[read_pos];
            o->delay_pos = (o->delay_pos + 1) % o->delay_len;

            float far_sample = delayed * far_gain;

            /* Assign to L/R based on which side the source is on */
            if (source_on_right) {
                output_binaural[0][i] += far_sample;  /* left ear = far */
                output_binaural[1][i] += near_sample; /* right ear = near */
            } else {
                output_binaural[0][i] += near_sample; /* left ear = near */
                output_binaural[1][i] += far_sample;  /* right ear = far */
            }
        }
    }

    return 0;
}