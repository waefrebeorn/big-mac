/* wb_ytpmv_vfx_pipeline.c — VFX Pipeline Integration (R132).
 *
 * Connects beat detection, VFX generation, and the production pipeline.
 * Generates complete ffmpeg filter_complex strings that include:
 * - Beat-synced video effects (zoom, shake, flash)
 * - Audio level-driven effects
 * - Transition effects between clips
 * - Multi-layer compositing with effects
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "wbus/wbus_compositor.h"

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* VPIPE_MAX_FILTER_LEN from original vfx file */
#define VPIPE_MAX_FILTER_LEN 8192
#define VPIPE_MAX_BEATS 1024

/* ================================================================
 * EFFECT TYPES
 * ================================================================ */

/* All types in wbus_compositor.h */

void vfx_beat_grid_init(vfx_beat_grid *grid, float bpm, float duration) {
    if (!grid) return;
    memset(grid, 0, sizeof(*grid));
    grid->bpm = bpm;
    grid->beat_interval = 60.0f / bpm;
    grid->n_beats = 0;
    for (float t = 0; t < duration && grid->n_beats < VPIPE_MAX_BEATS; t += grid->beat_interval) {
        grid->times[grid->n_beats++] = t;
    }
}

/* Generate beat times for subdivisions */
int vfx_beat_grid_subdivide(vfx_beat_grid *grid, int division, float *out_times, int max_out) {
    if (!grid || !out_times || division < 1) return 0;
    
    int count = 0;
    float sub_interval = grid->beat_interval / division;
    
    for (int i = 0; i < grid->n_beats && count < max_out; i++) {
        for (int d = 0; d < division && count < max_out; d++) {
            out_times[count++] = grid->times[i] + d * sub_interval;
        }
    }
    
    return count;
}

/* ================================================================
 * FILTER GENERATION
 * ================================================================ */

/* Generate zoom pulse filter string */
static int vfx_gen_zoom(vfx_effect *eff, vfx_beat_grid *grid, char *out, int max_len) {
    if (!eff || !grid || !out || max_len <= 0) return 0;
    
    float zoom_amount = eff->intensity * 0.15f; /* Max 15% zoom */
    float pulse_dur = 0.05f;
    
    int written = snprintf(out, max_len, "zoompan=z='%.2f", 1.0f + zoom_amount);
    
    if (eff->beat_synced && grid->n_beats > 0) {
        for (int i = 0; i < grid->n_beats && i < 64; i++) {
            float t = grid->times[i];
            written += snprintf(out + written, max_len - written,
                "+(gte(t,%.4f)*lt(t,%.4f)*(1.0-(t-%.4f)/%.4f)*%.2f)",
                t, t + pulse_dur, t, pulse_dur, zoom_amount);
        }
    }
    
    written += snprintf(out + written, max_len - written,
        "':d=1:x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)'");
    
    return written;
}

/* ================================================================
 * PIPELINE API
 * ================================================================ */

void vfx_pipeline_init(vfx_pipeline *pipe) {
    if (!pipe) return;
    memset(pipe, 0, sizeof(*pipe));
    pipe->bpm = 120.0f;
    pipe->epilepsy_safe = 1;
}

int vfx_pipeline_add_effect(vfx_pipeline *pipe, vfx_effect_type type,
                              float intensity, float freq, int beat_synced) {
    if (!pipe || pipe->n_effects >= 32) return -1;
    int idx = pipe->n_effects++;
    pipe->effects[idx].type = type;
    pipe->effects[idx].intensity = intensity;
    pipe->effects[idx].frequency = freq;
    pipe->effects[idx].beat_synced = beat_synced;
    pipe->effects[idx].start_time = 0.0f;
    pipe->effects[idx].duration = 0.0f;
    return idx;
}

/* Generate shake filter string */
static int vfx_gen_shake(vfx_effect *eff, vfx_beat_grid *grid, char *out, int max_len) {
    if (!eff || !out || max_len <= 0) return 0;
    
    float amt = eff->intensity * 10.0f; /* Max 10px shake */
    
    if (eff->beat_synced && grid && grid->n_beats > 0) {
        int written = snprintf(out, max_len, "translate=x='");
        for (int i = 0; i < grid->n_beats && i < 64; i++) {
            float t = grid->times[i];
            float phase = (float)((i * 7 + 3) % 13) / 13.0f * 2.0f * M_PI;
            if (i > 0) written += snprintf(out + written, max_len - written, "+");
            written += snprintf(out + written, max_len - written,
                "(gte(t,%.4f)*lt(t,%.1f)*sin(%.2f)*%.1f)", t, t + 0.1f, phase, amt);
        }
        written += snprintf(out + written, max_len - written, "':y='");
        for (int i = 0; i < grid->n_beats && i < 64; i++) {
            float t = grid->times[i];
            float phase = (float)((i * 11 + 5) % 17) / 17.0f * 2.0f * M_PI;
            if (i > 0) written += snprintf(out + written, max_len - written, "+");
            written += snprintf(out + written, max_len - written,
                "(gte(t,%.4f)*lt(t,%.1f)*cos(%.2f)*%.1f)", t, t + 0.1f, phase, amt);
        }
        written += snprintf(out + written, max_len - written, "'");
        return written;
    } else {
        return snprintf(out, max_len,
            "translate=x='sin(t*%.1f)*%.1f':y='cos(t*%.1f)*%.1f'",
            eff->frequency * 2.0f * M_PI, amt,
            eff->frequency * 2.0f * M_PI, amt);
    }
}

/* Generate flash filter string (epilepsy-safe) */
static int vfx_gen_flash(vfx_effect *eff, vfx_beat_grid *grid, char *out, int max_len, int safe) {
    if (!eff || !grid || !out || max_len <= 0) return 0;
    
    float flash_intensity = eff->intensity * 0.3f; /* Max 30% brightness boost */
    float flash_dur = 0.03f;
    
    /* Epilepsy safety: limit to 3 flashes/sec */
    float min_gap = 0.33f; /* 3 per second max */
    
    int written = snprintf(out, max_len, "geq=r='");
    float last_flash = -1.0f;
    
    for (int i = 0; i < grid->n_beats && i < 64; i++) {
        float t = grid->times[i];
        if (safe && t - last_flash < min_gap) continue;
        last_flash = t;
        
        if (i > 0) written += snprintf(out + written, max_len - written, "+");
        written += snprintf(out + written, max_len - written,
            "(gte(t,%.4f)*lt(t,%.4f)*(1.0-(t-%.4f)/%.4f)*%.2f)",
            t, t + flash_dur, t, flash_dur, flash_intensity * 255.0f);
    }
    
    written += snprintf(out + written, max_len - written, "':g='");
    last_flash = -1.0f;
    for (int i = 0; i < grid->n_beats && i < 64; i++) {
        float t = grid->times[i];
        if (safe && t - last_flash < min_gap) continue;
        last_flash = t;
        
        if (i > 0) written += snprintf(out + written, max_len - written, "+");
        written += snprintf(out + written, max_len - written,
            "(gte(t,%.4f)*lt(t,%.4f)*(1.0-(t-%.4f)/%.4f)*%.2f)",
            t, t + flash_dur, t, flash_dur, flash_intensity * 255.0f);
    }
    
    written += snprintf(out + written, max_len - written, "':b='");
    last_flash = -1.0f;
    for (int i = 0; i < grid->n_beats && i < 64; i++) {
        float t = grid->times[i];
        if (safe && t - last_flash < min_gap) continue;
        last_flash = t;
        
        if (i > 0) written += snprintf(out + written, max_len - written, "+");
        written += snprintf(out + written, max_len - written,
            "(gte(t,%.4f)*lt(t,%.4f)*(1.0-(t-%.4f)/%.4f)*%.2f)",
            t, t + flash_dur, t, flash_dur, flash_intensity * 255.0f);
    }
    
    written += snprintf(out + written, max_len - written, "'");
    return written;
}

/* Generate color grade filter string */
static int vfx_gen_color(vfx_effect *eff, vfx_beat_grid *grid, char *out, int max_len) {
    if (!eff || !out || max_len <= 0) return 0;
    
    float sat = 1.0f + eff->intensity * 0.5f; /* 1.0-1.5 saturation */
    float bright = 1.0f + eff->intensity * 0.1f; /* 1.0-1.1 brightness */
    
    return snprintf(out, max_len,
        "eq=saturation=%.2f:brightness=%.2f",
        sat, bright - 1.0f);
}

/* ================================================================
 * COMPLETE PIPELINE GENERATION
 * ================================================================ */

/* Generate the complete VFX filter chain */
int vfx_pipeline_generate(vfx_pipeline *pipe, vfx_beat_grid *grid,
                           char *output, int max_len) {
    if (!pipe || !grid || !output || max_len <= 0) return 0;
    
    int written = 0;
    output[0] = '\0';
    
    char temp[VPIPE_MAX_FILTER_LEN];
    
    for (int i = 0; i < pipe->n_effects; i++) {
        vfx_effect *eff = &pipe->effects[i];
        
        switch (eff->type) {
            case VFX_EFFECT_ZOOM_PULSE:
                vfx_gen_zoom(eff, grid, temp, sizeof(temp));
                break;
            case VFX_EFFECT_SHAKE:
                vfx_gen_shake(eff, grid, temp, sizeof(temp));
                break;
            case VFX_EFFECT_FLASH:
                vfx_gen_flash(eff, grid, temp, sizeof(temp), pipe->epilepsy_safe);
                break;
            case VFX_EFFECT_COLOR_GRADE:
                vfx_gen_color(eff, grid, temp, sizeof(temp));
                break;
            default:
                temp[0] = '\0';
                break;
        }
        
        if (temp[0] != '\0') {
            if (written > 0)
                written += snprintf(output + written, max_len - written, ",");
            written += snprintf(output + written, max_len - written, "%s", temp);
        }
    }
    
    return written;
}

/* ================================================================
 * CONVENIENCE PRESETS
 * ================================================================ */

/* Apply standard YTPMV effects preset */
void vfx_pipeline_preset_ytpmv(vfx_pipeline *pipe, float bpm, float duration, int epilepsy_safe) {
    if (!pipe) return;
    vfx_pipeline_init(pipe);
    pipe->bpm = bpm;
    pipe->total_duration = duration;
    pipe->epilepsy_safe = epilepsy_safe;
    
    /* Gentle zoom pulse on beat */
    vfx_pipeline_add_effect(pipe, VFX_EFFECT_ZOOM_PULSE, 0.5f, bpm/60.0f, 1);
    
    /* Subtle color grade */
    vfx_pipeline_add_effect(pipe, VFX_EFFECT_COLOR_GRADE, 0.3f, 0, 0);
}

/* Apply "intense" effects preset */
void vfx_pipeline_preset_intense(vfx_pipeline *pipe, float bpm, float duration, int epilepsy_safe) {
    if (!pipe) return;
    vfx_pipeline_init(pipe);
    pipe->bpm = bpm;
    pipe->total_duration = duration;
    pipe->epilepsy_safe = epilepsy_safe;
    
    vfx_pipeline_add_effect(pipe, VFX_EFFECT_ZOOM_PULSE, 0.8f, bpm/60.0f, 1);
    vfx_pipeline_add_effect(pipe, VFX_EFFECT_SHAKE, 0.6f, bpm/60.0f, 1);
    
    if (epilepsy_safe) {
        /* Reduced flash for safety */
        vfx_pipeline_add_effect(pipe, VFX_EFFECT_FLASH, 0.15f, bpm/60.0f, 1);
    } else {
        vfx_pipeline_add_effect(pipe, VFX_EFFECT_FLASH, 0.5f, bpm/60.0f, 1);
    }
    
    vfx_pipeline_add_effect(pipe, VFX_EFFECT_COLOR_GRADE, 0.5f, 0, 0);
}

/* Apply "minimal" effects preset (maximum epilepsy safety) */
void vfx_pipeline_preset_minimal(vfx_pipeline *pipe, float bpm, float duration) {
    if (!pipe) return;
    vfx_pipeline_init(pipe);
    pipe->bpm = bpm;
    pipe->total_duration = duration;
    pipe->epilepsy_safe = 1;
    
    /* Very gentle zoom only */
    vfx_pipeline_add_effect(pipe, VFX_EFFECT_ZOOM_PULSE, 0.2f, bpm/60.0f, 1);
}
