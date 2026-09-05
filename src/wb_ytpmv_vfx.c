/* wb_ytpmv_vfx.c — YTPMV Audio-Reactive Visual Effects Engine (R128).
 *
 * Generates ffmpeg filter strings for audio-reactive effects:
 * - Beat-synced zoom pulses
 * - Color flash on beat
 * - Screen shake
 * - RGB channel shift
 * - Volume-driven opacity
 *
 * The engine outputs ffmpeg filter_complex expressions that can be
 * piped directly into ffmpeg CLI calls.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

/* ================================================================
 * CONFIGURATION
 * ================================================================ */

#define VFX_MAX_FILTER_LEN 2048
#define VFX_MAX_BEATS 1024
#define VFX_MAX_KEYFRAMES 32768

/* ================================================================
 * BEAT TRACK
 * ================================================================ */

typedef struct {
    float times[VFX_MAX_BEATS];
    int n_beats;
    float bpm;
    float duration;
} vfx_beat_track;

void vfx_beat_track_init(vfx_beat_track *bt) {
    if (!bt) return;
    memset(bt, 0, sizeof(*bt));
    bt->bpm = 120.0f;
}

/* Generate beat times from BPM */
void vfx_beat_track_from_bpm(vfx_beat_track *bt, float bpm, float duration) {
    if (!bt) return;
    bt->bpm = bpm;
    bt->duration = duration;
    bt->n_beats = 0;
    
    float beat_interval = 60.0f / bpm;
    for (float t = 0; t < duration && bt->n_beats < VFX_MAX_BEATS; t += beat_interval) {
        bt->times[bt->n_beats++] = t;
    }
}

/* Detect beats from audio using energy flux (simplified) */
int vfx_detect_beats(const float *audio, int n_frames, int n_channels,
                      float sample_rate, vfx_beat_track *bt) {
    if (!audio || !bt || n_frames <= 0) return 0;
    
    int window_ms = 20;
    int window_size = (int)(sample_rate * window_ms / 1000.0f);
    if (window_size < 64) window_size = 64;
    int hop = window_size / 2;
    
    /* Calculate energy per window */
    int n_windows = (n_frames - window_size) / hop;
    if (n_windows <= 0) return 0;
    
    float *energy = (float *)calloc(n_windows, sizeof(float));
    float *flux = (float *)calloc(n_windows, sizeof(float));
    
    for (int i = 0; i < n_windows; i++) {
        double e = 0;
        for (int j = 0; j < window_size; j++) {
            float s = 0;
            for (int c = 0; c < n_channels; c++)
                s += audio[(i * hop + j) * n_channels + c];
            s /= n_channels;
            e += s * s;
        }
        energy[i] = (float)(e / window_size);
    }
    
    /* Spectral flux = positive energy difference */
    flux[0] = 0;
    for (int i = 1; i < n_windows; i++) {
        float diff = energy[i] - energy[i-1];
        flux[i] = diff > 0 ? diff : 0;
    }
    
    /* Adaptive threshold */
    float mean_flux = 0;
    for (int i = 0; i < n_windows; i++) mean_flux += flux[i];
    mean_flux /= n_windows;
    float threshold = mean_flux * 1.5f;
    
    /* Peak picking with minimum distance */
    float min_beat_gap = 0.15f; /* Minimum 150ms between beats */
    float last_beat_time = -1.0f;
    bt->n_beats = 0;
    
    for (int i = 1; i < n_windows - 1; i++) {
        float t = (float)(i * hop) / sample_rate;
        if (flux[i] > threshold && flux[i] > flux[i-1] && flux[i] > flux[i+1]) {
            if (t - last_beat_time >= min_beat_gap) {
                bt->times[bt->n_beats++] = t;
                last_beat_time = t;
                if (bt->n_beats >= VFX_MAX_BEATS) break;
            }
        }
    }
    
    /* Calculate BPM from beat intervals */
    if (bt->n_beats >= 2) {
        float total_interval = 0;
        for (int i = 1; i < bt->n_beats; i++)
            total_interval += bt->times[i] - bt->times[i-1];
        float avg_interval = total_interval / (bt->n_beats - 1);
        bt->bpm = 60.0f / avg_interval;
    }
    
    free(energy);
    free(flux);
    return bt->n_beats;
}

/* ================================================================
 * ZOOM PULSE GENERATOR
 * ================================================================ */

typedef struct {
    float amount;          /* Zoom factor (0.0 = no zoom, 0.2 = 20% zoom) */
    float duration;        /* Duration of each pulse in seconds */
    float base_zoom;       /* Base zoom level (1.0 = normal) */
} vfx_zoom_pulse_config;

void vfx_zoom_pulse_config_init(vfx_zoom_pulse_config *cfg) {
    if (!cfg) return;
    cfg->amount = 0.08f;
    cfg->duration = 0.05f;
    cfg->base_zoom = 1.0f;
}

/* Generate zoompan filter string for beat-synced zoom pulse */
int vfx_generate_zoom_pulse(vfx_zoom_pulse_config *cfg, vfx_beat_track *bt,
                             char *output, int max_len) {
    if (!cfg || !bt || !output || max_len <= 0) return 0;
    
    /* 
     * zoompan expression: zoom to (base + amount) at each beat, 
     * return to base after pulse_duration
     * 
     * For each beat at time T: if (T <= t < T+dur) zoom = base + amount * (1 - (t-T)/dur)
     * This creates a smooth zoom-in then zoom-out effect
     */
    
    int written = 0;
    written += snprintf(output + written, max_len - written,
                        "zoompan=z='");
    
    /* Build conditional zoom expression */
    for (int i = 0; i < bt->n_beats && i < 64; i++) {
        float beat_time = bt->times[i];
        float pulse_dur = cfg->duration;
        float zoom_max = cfg->base_zoom + cfg->amount;
        
        if (i > 0)
            written += snprintf(output + written, max_len - written, "+");
        
        /* Between(beat_time, t, beat_time+pulse_dur) * zoom_amount * envelope */
        written += snprintf(output + written, max_len - written,
                            "(gte(t,%.4f)*lt(t,%.4f)*(1.0-(t-%.4f)/%.4f)*%.2f)",
                            beat_time, beat_time + pulse_dur, beat_time, pulse_dur, cfg->amount);
    }
    
    if (bt->n_beats == 0) {
        written += snprintf(output + written, max_len - written, "%.2f", cfg->base_zoom);
    } else {
        written += snprintf(output + written, max_len - written, "+%.2f", cfg->base_zoom);
    }
    
    written += snprintf(output + written, max_len - written,
                        "':d=1:x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)'");
    
    return written;
}

/* ================================================================
 * SCREEN SHAKE GENERATOR
 * ================================================================ */

typedef struct {
    float intensity;       /* Shake amplitude in pixels */
    float frequency;       /* Shake frequency in Hz */
    float beat_synced;     /* If true, shake only on beats */
} vfx_shake_config;

void vfx_shake_config_init(vfx_shake_config *cfg) {
    if (!cfg) return;
    cfg->intensity = 5.0f;
    cfg->frequency = 20.0f;
    cfg->beat_synced = 1;
}

/* Generate translate filter string for screen shake */
int vfx_generate_shake(vfx_shake_config *cfg, vfx_beat_track *bt,
                        char *output, int max_len) {
    if (!cfg || !output || max_len <= 0) return 0;
    
    if (cfg->beat_synced && bt && bt->n_beats > 0) {
        /* Beat-synced shake: random offset on each beat */
        int written = 0;
        written += snprintf(output + written, max_len - written,
                            "translate=x='");
        
        for (int i = 0; i < bt->n_beats && i < 64; i++) {
            float beat_time = bt->times[i];
            float shake_dur = 0.1f;
            /* Use deterministic "random" based on beat index */
            float phase1 = (float)((i * 7 + 3) % 13) / 13.0f * 2.0f * M_PI;
            float phase2 = (float)((i * 11 + 5) % 17) / 17.0f * 2.0f * M_PI;
            
            if (i > 0)
                written += snprintf(output + written, max_len - written, "+");
            
            written += snprintf(output + written, max_len - written,
                                "(gte(t,%.4f)*lt(t,%.4f)*sin(%.2f)*%.1f)",
                                beat_time, beat_time + shake_dur, phase1, cfg->intensity);
        }
        
        written += snprintf(output + written, max_len - written,
                            "':y='");
        
        for (int i = 0; i < bt->n_beats && i < 64; i++) {
            float beat_time = bt->times[i];
            float shake_dur = 0.1f;
            float phase2 = (float)((i * 11 + 5) % 17) / 17.0f * 2.0f * M_PI;
            
            if (i > 0)
                written += snprintf(output + written, max_len - written, "+");
            
            written += snprintf(output + written, max_len - written,
                                "(gte(t,%.4f)*lt(t,%.4f)*cos(%.2f)*%.1f)",
                                beat_time, beat_time + shake_dur, phase2, cfg->intensity);
        }
        
        written += snprintf(output + written, max_len - written, "'");
        return written;
    } else {
        /* Continuous shake */
        return snprintf(output, max_len,
                        "translate=x='sin(t*%.1f)*%.1f':y='cos(t*%.1f)*%.1f'",
                        cfg->frequency * 2.0f * M_PI, cfg->intensity,
                        cfg->frequency * 2.0f * M_PI, cfg->intensity);
    }
}

/* ================================================================
 * COLOR FLASH GENERATOR
 * ================================================================ */

typedef struct {
    float intensity;       /* Flash brightness (0.0-1.0) */
    float duration;        /* Flash duration in seconds */
    float r, g, b;         /* Flash color (0-255) */
} vfx_flash_config;

void vfx_flash_config_init(vfx_flash_config *cfg) {
    if (!cfg) return;
    cfg->intensity = 0.3f;
    cfg->duration = 0.03f;
    cfg->r = 255; cfg->g = 255; cfg->b = 255;
}

/* Generate geq filter for color flash on beat */
int vfx_generate_flash(vfx_flash_config *cfg, vfx_beat_track *bt,
                        char *output, int max_len) {
    if (!cfg || !bt || !output || max_len <= 0) return 0;
    
    int written = 0;
    written += snprintf(output + written, max_len - written, "geq=");
    
    /* Red channel */
    written += snprintf(output + written, max_len - written, "r='");
    for (int i = 0; i < bt->n_beats && i < 64; i++) {
        float beat_time = bt->times[i];
        float flash_dur = cfg->duration;
        if (i > 0)
            written += snprintf(output + written, max_len - written, "+");
        written += snprintf(output + written, max_len - written,
                            "(gte(t,%.4f)*lt(t,%.4f)*(1.0-(t-%.4f)/%.4f)*%.0f*%.2f)",
                            beat_time, beat_time + flash_dur, beat_time, flash_dur,
                            (int)cfg->r, cfg->intensity);
    }
    written += snprintf(output + written, max_len - written, "'");
    
    /* Green channel */
    written += snprintf(output + written, max_len - written, ":g='");
    for (int i = 0; i < bt->n_beats && i < 64; i++) {
        float beat_time = bt->times[i];
        float flash_dur = cfg->duration;
        if (i > 0)
            written += snprintf(output + written, max_len - written, "+");
        written += snprintf(output + written, max_len - written,
                            "(gte(t,%.4f)*lt(t,%.4f)*(1.0-(t-%.4f)/%.4f)*%.0f*%.2f)",
                            beat_time, beat_time + flash_dur, beat_time, flash_dur,
                            (int)cfg->g, cfg->intensity);
    }
    written += snprintf(output + written, max_len - written, "'");
    
    /* Blue channel */
    written += snprintf(output + written, max_len - written, ":b='");
    for (int i = 0; i < bt->n_beats && i < 64; i++) {
        float beat_time = bt->times[i];
        float flash_dur = cfg->duration;
        if (i > 0)
            written += snprintf(output + written, max_len - written, "+");
        written += snprintf(output + written, max_len - written,
                            "(gte(t,%.4f)*lt(t,%.4f)*(1.0-(t-%.4f)/%.4f)*%.0f*%.2f)",
                            beat_time, beat_time + flash_dur, beat_time, flash_dur,
                            (int)cfg->b, cfg->intensity);
    }
    written += snprintf(output + written, max_len - written, "'");
    
    return written;
}

/* ================================================================
 * RGB CHANNEL SHIFT
 * ================================================================ */

typedef struct {
    float max_offset;      /* Maximum pixel offset */
    float beat_synced;     /* If true, shift on beats */
} vfx_rgb_shift_config;

void vfx_rgb_shift_config_init(vfx_rgb_shift_config *cfg) {
    if (!cfg) return;
    cfg->max_offset = 4.0f;
    cfg->beat_synced = 1;
}

/* Generate RGB shift filter string */
int vfx_generate_rgb_shift(vfx_rgb_shift_config *cfg, vfx_beat_track *bt,
                            char *output, int max_len) {
    if (!cfg || !output || max_len <= 0) return 0;
    
    if (cfg->beat_synced && bt && bt->n_beats > 0) {
        /* Beat-synced RGB shift */
        return snprintf(output, max_len,
                        "split=3[r][g][b];"
                        "[r]crop=iw:ih:0:0,geq=r='r(X,Y)':g='0':b='0',pad=iw+%d:ih+%d:%d:%d[red];"
                        "[g]crop=ih:ih:0:0,geq=r='0':g='g(X,Y)':b='0',pad=iw+%d:ih+%d:%d:%d[green];"
                        "[b]crop=ih:ih:0:0,geq=r='0':g='0':b='b(X,Y)',pad=iw+%d:ih+%d:%d:%d[blue];"
                        "[red][green][blue]blend=all_mode=addition",
                        (int)cfg->max_offset*2, (int)cfg->max_offset*2,
                        (int)cfg->max_offset, (int)cfg->max_offset,
                        (int)cfg->max_offset*2, (int)cfg->max_offset*2,
                        (int)cfg->max_offset, (int)cfg->max_offset,
                        (int)cfg->max_offset*2, (int)cfg->max_offset*2,
                        (int)cfg->max_offset, (int)cfg->max_offset);
    } else {
        /* Static RGB shift */
        return snprintf(output, max_len,
                        "split=3[r][g][b];"
                        "[r]crop=iw:ih:0:0,geq=r='r(X,Y)':g='0':b='0',pad=iw+%d:ih+%d:%d:%d[red];"
                        "[g]crop=ih:ih:%d:0,geq=r='0':g='g(X,Y)':b='0',pad=iw+%d:ih+%d:%d:%d[green];"
                        "[b]crop=ih:ih:%d:0,geq=r='0':g='0':b='b(X,Y)',pad=iw+%d:ih+%d:%d:%d[blue];"
                        "[red][green][blue]blend=all_mode=addition",
                        (int)cfg->max_offset*2, (int)cfg->max_offset*2, (int)cfg->max_offset, (int)cfg->max_offset,
                        (int)cfg->max_offset, (int)cfg->max_offset*2, (int)cfg->max_offset*2, (int)cfg->max_offset, (int)cfg->max_offset,
                        (int)cfg->max_offset*2, (int)cfg->max_offset*2, (int)cfg->max_offset, (int)cfg->max_offset);
    }
}

/* ================================================================
 * COMBINED EFFECTS PIPELINE
 * ================================================================ */

typedef struct {
    vfx_beat_track beat_track;
    vfx_zoom_pulse_config zoom;
    vfx_shake_config shake;
    vfx_flash_config flash;
    vfx_rgb_shift_config rgb_shift;
    
    int enable_zoom : 1;
    int enable_shake : 1;
    int enable_flash : 1;
    int enable_rgb_shift : 1;
    
    char filter_chain[VFX_MAX_KEYFRAMES];
} vfx_pipeline;

void vfx_pipeline_init(vfx_pipeline *pipe) {
    if (!pipe) return;
    memset(pipe, 0, sizeof(*pipe));
    vfx_beat_track_init(&pipe->beat_track);
    vfx_zoom_pulse_config_init(&pipe->zoom);
    vfx_shake_config_init(&pipe->shake);
    vfx_flash_config_init(&pipe->flash);
    vfx_rgb_shift_config_init(&pipe->rgb_shift);
}

/* Build the complete filter chain from all enabled effects */
int vfx_pipeline_build(vfx_pipeline *pipe) {
    if (!pipe) return 0;
    
    int written = 0;
    pipe->filter_chain[0] = '\0';
    
    char temp[VFX_MAX_FILTER_LEN];
    
    if (pipe->enable_zoom) {
        vfx_generate_zoom_pulse(&pipe->zoom, &pipe->beat_track, temp, sizeof(temp));
        written += snprintf(pipe->filter_chain + written, sizeof(pipe->filter_chain) - written, "%s", temp);
    }
    
    if (pipe->enable_shake) {
        if (written > 0)
            written += snprintf(pipe->filter_chain + written, sizeof(pipe->filter_chain) - written, ",");
        vfx_generate_shake(&pipe->shake, &pipe->beat_track, temp, sizeof(temp));
        written += snprintf(pipe->filter_chain + written, sizeof(pipe->filter_chain) - written, "%s", temp);
    }
    
    if (pipe->enable_flash) {
        if (written > 0)
            written += snprintf(pipe->filter_chain + written, sizeof(pipe->filter_chain) - written, ",");
        vfx_generate_flash(&pipe->flash, &pipe->beat_track, temp, sizeof(temp));
        written += snprintf(pipe->filter_chain + written, sizeof(pipe->filter_chain) - written, "%s", temp);
    }
    
    if (pipe->enable_rgb_shift) {
        if (written > 0)
            written += snprintf(pipe->filter_chain + written, sizeof(pipe->filter_chain) - written, ",");
        vfx_generate_rgb_shift(&pipe->rgb_shift, &pipe->beat_track, temp, sizeof(temp));
        written += snprintf(pipe->filter_chain + written, sizeof(pipe->filter_chain) - written, "%s", temp);
    }
    
    return written;
}

/* ================================================================
 * KEYFRAME GENERATION (for external tools)
 * ================================================================ */

/* Generate a keyframe file for ffmpeg's -f concat or complex filter */
int vfx_generate_keyframes(vfx_beat_track *bt, const char *effect_type,
                            float *values, int max_values) {
    if (!bt || !values || max_values <= 0) return 0;
    
    int count = 0;
    for (int i = 0; i < bt->n_beats && count < max_values; i++) {
        float t = bt->times[i];
        
        if (strcmp(effect_type, "zoom") == 0) {
            /* Zoom keyframes: 1.0 at beat start, peak at +0.02s, back at +0.05s */
            values[count++] = t;
            values[count++] = 1.0f;
            values[count++] = t + 0.02f;
            values[count++] = 1.08f;
            values[count++] = t + 0.05f;
            values[count++] = 1.0f;
        } else if (strcmp(effect_type, "flash") == 0) {
            /* Flash keyframes: 0 at beat start, peak at +0.01s, back at +0.03s */
            values[count++] = t;
            values[count++] = 0.0f;
            values[count++] = t + 0.01f;
            values[count++] = 0.3f;
            values[count++] = t + 0.03f;
            values[count++] = 0.0f;
        }
    }
    
    return count;
}
