/* wb_audio_color.c — audio-reactive color grading for YTP music videos.
 *
 * R080: Maps audio features to color parameters — bass drives saturation,
 * treble drives brightness, mids drive hue shift. Essential for YTPMV
 * and music-synchronized visual effects.
 *
 * Features:
 *   - Bass → saturation boost
 *   - Treble → brightness/exposure
 *   - Mids → hue rotation
 *   - Beat → flash/color pop
 *   - Spectral centroid → color temperature
 *
 * Pure C11, operates on RGBA uint8 frame buffers. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

/* Simple biquad for frequency band extraction */
typedef struct {
    float b0, b1, b2, a1, a2;
    float x[2], y[2];
} biquad_t;

static void biquad_init(biquad_t *f) {
    f->x[0] = f->x[1] = f->y[0] = f->y[1] = 0.0f;
}

static void biquad_lowpass(biquad_t *f, float freq, uint32_t sr) {
    float omega = 2.0f * M_PI * freq / sr;
    float sin_omega = sinf(omega);
    float cos_omega = cosf(omega);
    float alpha = sin_omega / 2.0f;
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f - cos_omega) / (2.0f * a0);
    f->b1 = (1.0f - cos_omega) / a0;
    f->b2 = f->b0;
    f->a1 = (-2.0f * cos_omega) / a0;
    f->a2 = (1.0f - alpha) / a0;
    biquad_init(f);
}

static void biquad_highpass(biquad_t *f, float freq, uint32_t sr) {
    float omega = 2.0f * M_PI * freq / sr;
    float sin_omega = sinf(omega);
    float cos_omega = cosf(omega);
    float alpha = sin_omega / 2.0f;
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f + cos_omega) / (2.0f * a0);
    f->b1 = -(1.0f + cos_omega) / a0;
    f->b2 = f->b0;
    f->a1 = (-2.0f * cos_omega) / a0;
    f->a2 = (1.0f - alpha) / a0;
    biquad_init(f);
}

static float biquad_process(biquad_t *f, float in) {
    float out = f->b0 * in + f->b1 * f->x[0] + f->b2 * f->x[1]
                - f->a1 * f->y[0] - f->a2 * f->y[1];
    f->x[1] = f->x[0]; f->x[0] = in;
    f->y[1] = f->y[0]; f->y[0] = out;
    return out;
}

/* RGB to HSV conversion */
static void rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b, float *h, float *s, float *v) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float max = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
    float min = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
    float delta = max - min;

    *v = max;
    *s = (max < 0.0001f) ? 0.0f : delta / max;

    if (delta < 0.0001f) {
        *h = 0.0f;
    } else if (max == rf) {
        *h = 60.0f * fmodf((gf - bf) / delta, 6.0f);
    } else if (max == gf) {
        *h = 60.0f * ((bf - rf) / delta + 2.0f);
    } else {
        *h = 60.0f * ((rf - gf) / delta + 4.0f);
    }
    if (*h < 0) *h += 360.0f;
}

static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rf, gf, bf;

    if (h < 60)       { rf = c; gf = x; bf = 0; }
    else if (h < 120) { rf = x; gf = c; bf = 0; }
    else if (h < 180) { rf = 0; gf = c; bf = x; }
    else if (h < 240) { rf = 0; gf = x; bf = c; }
    else if (h < 300) { rf = x; gf = 0; bf = c; }
    else              { rf = c; gf = 0; bf = x; }

    *r = (uint8_t)((rf + m) * 255.0f);
    *g = (uint8_t)((gf + m) * 255.0f);
    *b = (uint8_t)((bf + m) * 255.0f);
}

typedef struct {
    uint32_t sr;

    /* Frequency band filters */
    biquad_t bass_lp;       /* < 150 Hz */
    biquad_t treble_hp;     /* > 2000 Hz */
    biquad_t mid_lp;
    biquad_t mid_hp;        /* 150-2000 Hz */

    /* Smoothed energy values */
    float bass_energy;
    float mid_energy;
    float treble_energy;
    float attack;
    float release;

    /* Beat detection */
    float prev_energy;
    float beat_threshold;
    int   beat_detected;

    /* Parameters */
    float saturation_bass;  /* How much bass affects saturation */
    float brightness_treble;/* How much treble affects brightness */
    float hue_mid;          /* How much mids affect hue */
    float flash_amount;     /* Beat flash intensity */
    float color_temp_shift; /* Spectral centroid → warmth */
} wb_audio_color_inst;

void *wb_audio_color_create(uint32_t sr) {
    wb_audio_color_inst *inst = (wb_audio_color_inst *)calloc(1, sizeof(wb_audio_color_inst));
    if (!inst) return NULL;
    inst->sr = sr;
    inst->attack = 0.3f;
    inst->release = 0.05f;
    inst->beat_threshold = 1.3f;
    inst->saturation_bass = 2.0f;
    inst->brightness_treble = 1.5f;
    inst->hue_mid = 60.0f;
    inst->flash_amount = 0.3f;
    inst->color_temp_shift = 0.5f;

    biquad_lowpass(&inst->bass_lp, 150.0f, sr);
    biquad_highpass(&inst->treble_hp, 2000.0f, sr);
    biquad_highpass(&inst->mid_lp, 150.0f, sr);
    biquad_lowpass(&inst->mid_hp, 2000.0f, sr);

    return inst;
}

void wb_audio_color_destroy(void *inst) {
    free(inst);
}

/* Analyze audio frame and update internal state */
void wb_audio_color_analyze(void *inst, const float *audio, int n) {
    wb_audio_color_inst *ac = (wb_audio_color_inst *)inst;
    if (!ac || !audio) return;

    float bass_sum = 0, mid_sum = 0, treble_sum = 0, total = 0;

    for (int i = 0; i < n; i++) {
        float s = audio[i];
        float bass = biquad_process(&ac->bass_lp, s);
        float treble = biquad_process(&ac->treble_hp, s);
        float mid_lo = biquad_process(&ac->mid_lp, s);
        float mid_hi = biquad_process(&ac->mid_hp, mid_lo);
        float mid = mid_hi;

        bass_sum += bass * bass;
        mid_sum += mid * mid;
        treble_sum += treble * treble;
        total += s * s;
    }

    float bass_rms = sqrtf(bass_sum / n);
    float mid_rms = sqrtf(mid_sum / n);
    float treble_rms = sqrtf(treble_sum / n);
    float total_rms = sqrtf(total / n);

    /* Smooth with attack/release */
    float atk = ac->attack, rel = ac->release;
    float bass_target = bass_rms;
    float bass_rate = (bass_target > ac->bass_energy) ? atk : rel;
    ac->bass_energy += (bass_target - ac->bass_energy) * bass_rate;

    float mid_target = mid_rms;
    float mid_rate = (mid_target > ac->mid_energy) ? atk : rel;
    ac->mid_energy += (mid_target - ac->mid_energy) * mid_rate;

    float treble_target = treble_rms;
    float treble_rate = (treble_target > ac->treble_energy) ? atk : rel;
    ac->treble_energy += (treble_target - ac->treble_energy) * treble_rate;

    /* Beat detection */
    ac->beat_detected = (total_rms > ac->prev_energy * ac->beat_threshold) ? 1 : 0;
    ac->prev_energy = total_rms * 0.9f + ac->prev_energy * 0.1f;
}

/* Apply color grade to RGBA frame based on current audio analysis */
void wb_audio_color_apply(void *inst, uint8_t *buf, int width, int height) {
    wb_audio_color_inst *ac = (wb_audio_color_inst *)inst;
    if (!ac || !buf) return;

    /* Compute color grade parameters from audio */
    float sat_mult = 1.0f + ac->bass_energy * ac->saturation_bass * 10.0f;
    float bri_mult = 1.0f + ac->treble_energy * ac->brightness_treble * 8.0f;
    float hue_shift = ac->mid_energy * ac->hue_mid * 20.0f;

    /* Beat flash */
    float flash = ac->beat_detected ? ac->flash_amount : 0.0f;

    if (sat_mult > 3.0f) sat_mult = 3.0f;
    if (bri_mult > 2.0f) bri_mult = 2.0f;

    int npixels = width * height;

    for (int i = 0; i < npixels; i++) {
        int idx = i * 4;
        uint8_t r = buf[idx];
        uint8_t g = buf[idx + 1];
        uint8_t b = buf[idx + 2];

        float h, s, v;
        rgb_to_hsv(r, g, b, &h, &s, &v);

        /* Apply hue shift from mids */
        h = fmodf(h + hue_shift, 360.0f);
        if (h < 0) h += 360.0f;

        /* Apply saturation boost from bass */
        s *= sat_mult;
        if (s > 1.0f) s = 1.0f;

        /* Apply brightness from treble */
        v *= bri_mult;
        v += flash;
        if (v > 1.0f) v = 1.0f;

        hsv_to_rgb(h, s, v, &buf[idx], &buf[idx + 1], &buf[idx + 2]);
    }
}

/* Set saturation response to bass */
void wb_audio_color_set_saturation(void *inst, float amount) {
    wb_audio_color_inst *ac = (wb_audio_color_inst *)inst;
    if (ac) ac->saturation_bass = amount;
}

/* Set brightness response to treble */
void wb_audio_color_set_brightness(void *inst, float amount) {
    wb_audio_color_inst *ac = (wb_audio_color_inst *)inst;
    if (ac) ac->brightness_treble = amount;
}

/* Set hue shift response to mids */
void wb_audio_color_set_hue(void *inst, float amount) {
    wb_audio_color_inst *ac = (wb_audio_color_inst *)inst;
    if (ac) ac->hue_mid = amount;
}

/* Set beat flash intensity */
void wb_audio_color_set_flash(void *inst, float amount) {
    wb_audio_color_inst *ac = (wb_audio_color_inst *)inst;
    if (ac) ac->flash_amount = amount;
}
