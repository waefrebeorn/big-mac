/* wb_audio_reactive.c — audio-reactive video effect engine.
 *
 * R077: Maps audio features to visual parameters for music video generation.
 *
 * Features extracted:
 *   - Bass energy (20-150Hz)
 *   - Mid energy (150-2000Hz)
 *   - Treble energy (2000-20000Hz)
 *   - Overall RMS
 *   - Beat detection (onset)
 *   - Spectral centroid (brightness)
 *
 * Outputs:
 *   - Zoom amount (bass-driven)
 *   - Color shift (frequency-driven)
 *   - Flash trigger (beat-driven)
 *   - Shake amount (energy-driven)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

/* Simple 3-band energy extractor using running filters */
typedef struct {
    uint32_t sr;

    /* Band energies (smoothed) */
    float    bass_energy;     /* 20-150 Hz */
    float    mid_energy;      /* 150-2000 Hz */
    float    treble_energy;   /* 2000-20000 Hz */
    float    total_rms;

    /* Filter states (simple 1-pole) */
    float    bass_state[4];   /* Biquad for bass band */
    float    mid_state[4];    /* Biquad for mid band */
    float    treble_state[4];  /* Biquad for treble band */

    /* Beat detection */
    float    prev_energy;
    float    beat_pulse;       /* Decays after each beat */
    float    beat_threshold;

    /* Smoothing coefficients */
    float    attack_coeff;
    float    release_coeff;

    /* Output parameters (0..1 normalized) */
    float    out_zoom;         /* Bass-driven zoom (0=normal, 1=max zoom) */
    float    out_flash;        /* Beat-triggered flash (0=none, 1=full white) */
    float    out_shake;        /* Energy-driven shake amount */
    float    out_color_shift;  /* Spectral centroid -> hue shift */
    float    out_brightness;   /* Overall energy -> brightness */

} wb_audio_reactive_inst;

void *wb_audio_reactive_create(uint32_t sr) {
    wb_audio_reactive_inst *ar = (wb_audio_reactive_inst *)calloc(1, sizeof(*ar));
    if (!ar) return NULL;
    ar->sr = sr;
    ar->attack_coeff = expf(-1.0f / (5.0f * 0.001f * sr));    /* 5ms attack */
    ar->release_coeff = expf(-1.0f / (50.0f * 0.001f * sr));   /* 50ms release */
    ar->beat_threshold = 1.3f;  /* 30% above average = beat */
    ar->prev_energy = 0.0f;
    ar->beat_pulse = 0.0f;

    /* Initialize filter coefficients would go here */
    /* For now, use simple energy-based extraction */

    return ar;
}

void wb_audio_reactive_destroy(void *inst) {
    free(inst);
}

/* Process audio features and update visual parameters.
 * mono: mono audio buffer (can be L+R mixed)
 * n: number of samples */
void wb_audio_reactive_update(wb_audio_reactive_inst *ar,
                               const float *mono, int n) {
    if (!ar || !mono || n < 1) return;

    /* Compute band energies (simplified: use time-domain approximation) */
    float bass_sum = 0, mid_sum = 0, treble_sum = 0, total_sum = 0;

    for (int i = 0; i < n; i++) {
        float s = mono[i];
        float abs_s = fabsf(s);

        /* Simple band separation using running difference for treble */
        float diff = (i > 0) ? fabsf(s - mono[i-1]) : 0;
        float low = fabsf(s) - diff * 0.5f;
        if (low < 0) low = 0;

        bass_sum += low * low;
        treble_sum += diff * diff;
        mid_sum += abs_s * abs_s - low * low - diff * diff;
        if (mid_sum < 0) mid_sum = 0;
        total_sum += s * s;
    }

    float n_f = (float)n;
    float bass_rms = sqrtf(bass_sum / n_f);
    float mid_rms = sqrtf(mid_sum / n_f);
    float treble_rms = sqrtf(treble_sum / n_f);
    float total_rms = sqrtf(total_sum / n_f);

    /* Smooth energies */
    ar->bass_energy = ar->attack_coeff * ar->bass_energy + (1.0f - ar->attack_coeff) * bass_rms;
    ar->mid_energy = ar->attack_coeff * ar->mid_energy + (1.0f - ar->attack_coeff) * mid_rms;
    ar->treble_energy = ar->attack_coeff * ar->treble_energy + (1.0f - ar->attack_coeff) * treble_rms;
    ar->total_rms = ar->release_coeff * ar->total_rms + (1.0f - ar->release_coeff) * total_rms;

    /* Beat detection */
    if (ar->total_rms > ar->prev_energy * ar->beat_threshold) {
        ar->beat_pulse = 1.0f;
    }
    ar->prev_energy = ar->total_rms * 0.9f + ar->prev_energy * 0.1f; /* running average */

    /* Decay beat pulse */
    ar->beat_pulse *= 0.95f;

    /* Map to visual outputs */
    /* Zoom: bass energy (normalize to 0..1, clamp) */
    ar->out_zoom = ar->bass_energy * 10.0f;
    if (ar->out_zoom > 1.0f) ar->out_zoom = 1.0f;

    /* Flash: beat pulse */
    ar->out_flash = ar->beat_pulse;

    /* Shake: overall energy */
    ar->out_shake = ar->total_rms * 5.0f;
    if (ar->out_shake > 1.0f) ar->out_shake = 1.0f;

    /* Color shift: spectral centroid approximation */
    float total_energy = ar->bass_energy + ar->mid_energy + ar->treble_energy + 1e-10f;
    ar->out_color_shift = (ar->treble_energy * 2.0f + ar->mid_energy) / total_energy / 3.0f;

    /* Brightness: overall energy */
    ar->out_brightness = ar->total_rms * 3.0f;
    if (ar->out_brightness > 1.0f) ar->out_brightness = 1.0f;
}

/* Get current zoom factor (1.0 = no zoom, 2.0 = 2x zoom) */
float wb_audio_reactive_get_zoom(wb_audio_reactive_inst *ar) {
    return 1.0f + (ar ? ar->out_zoom : 0.0f) * 0.5f;
}

/* Get current flash amount (0..1) */
float wb_audio_reactive_get_flash(wb_audio_reactive_inst *ar) {
    return ar ? ar->out_flash : 0.0f;
}

/* Get current shake amount (0..1) */
float wb_audio_reactive_get_shake(wb_audio_reactive_inst *ar) {
    return ar ? ar->out_shake : 0.0f;
}

/* Get current color shift (0..1 -> 0..360 hue) */
float wb_audio_reactive_get_color_shift(wb_audio_reactive_inst *ar) {
    return ar ? ar->out_color_shift * 360.0f : 0.0f;
}

/* Get current brightness multiplier (0.5..1.5) */
float wb_audio_reactive_get_brightness(wb_audio_reactive_inst *ar) {
    if (!ar) return 1.0f;
    return 0.7f + ar->out_brightness * 0.5f;
}

/* Process a video frame: apply audio-reactive effects to RGBA pixels.
 * Applies: zoom (crop + scale), brightness, color shift, flash overlay.
 * frame: RGBA pixel data (modified in place)
 * width, height: frame dimensions */
void wb_audio_reactive_apply_frame(wb_audio_reactive_inst *ar,
                                    uint8_t *frame, int width, int height) {
    if (!ar || !frame) return;

    float zoom = wb_audio_reactive_get_zoom(ar);
    float flash = wb_audio_reactive_get_flash(ar);
    float brightness = wb_audio_reactive_get_brightness(ar);
    float color_shift = wb_audio_reactive_get_color_shift(ar);

    int n_pixels = width * height;

    /* Apply brightness + color shift */
    for (int i = 0; i < n_pixels; i++) {
        int idx = i * 4;

        /* Brightness */
        float r = (float)frame[idx] * brightness;
        float g = (float)frame[idx+1] * brightness;
        float b = (float)frame[idx+2] * brightness;

        /* Color shift (simple hue rotation approximation) */
        if (color_shift > 1.0f) {
            /* Shift towards warm (more R, less B) */
            float shift = (color_shift - 1.0f) * 0.3f;
            r += shift * 50.0f;
            b -= shift * 30.0f;
        }

        /* Flash overlay */
        if (flash > 0.01f) {
            r += flash * (255.0f - r);
            g += flash * (255.0f - g);
            b += flash * (255.0f - b);
        }

        /* Clamp */
        frame[idx]   = (uint8_t)(r > 255 ? 255 : (r < 0 ? 0 : r));
        frame[idx+1] = (uint8_t)(g > 255 ? 255 : (g < 0 ? 0 : g));
        frame[idx+2] = (uint8_t)(b > 255 ? 255 : (b < 0 ? 0 : b));
    }

    /* Zoom is handled by the compositor/rasterizer (crop + scale) */
    /* The zoom factor is read by wb_audio_reactive_get_zoom() */
}
