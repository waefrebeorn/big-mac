/* wb_audio_mixer.c — audio mixer for video editing (R086).
 *
 * Vegas 10 feature: per-track audio mixer with volume, pan, mute/solo,
 * and VU level meters.
 *
 * This module provides the mixer state and processing for the edit
 * graph's audio tracks. The actual UI rendering happens in the DAW.
 *
 * Pure C11.
 */

#include "wbus/wbus_edit.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Mixer state for each track */
typedef struct {
    float volume;       /* 0..2 linear (1.0 = unity) */
    float pan;          /* -1 (left) .. +1 (right), 0 = center */
    int muted;
    int soloed;
    float vu_level;     /* current VU level (0..1, for UI display) */
    float peak_level;   /* peak hold (0..1) */
    int input_bus;      /* input bus assignment (0 = default) */
} wb_mixer_track_t;

#define MAX_MIXER_TRACKS 32
#define MAX_INPUT_BUSES 26  /* Vegas 10 has 26 input buses */

static wb_mixer_track_t g_mixer_tracks[MAX_MIXER_TRACKS];
static int g_mixer_count = 0;
static float g_master_volume = 1.0f;
static float g_master_vu = 0.0f;
static int g_input_bus_enabled[MAX_INPUT_BUSES];

void wb_audio_mixer_init(void) {
    memset(g_mixer_tracks, 0, sizeof(g_mixer_tracks));
    for (int i = 0; i < MAX_MIXER_TRACKS; i++) {
        g_mixer_tracks[i].volume = 1.0f;
        g_mixer_tracks[i].pan = 0.0f;
    }
    memset(g_input_bus_enabled, 0, sizeof(g_input_bus_enabled));
    g_mixer_count = 0;
    g_master_volume = 1.0f;
}

/* Initialize mixer tracks for an edit graph */
void wb_audio_mixer_sync(wb_edit_graph *g) {
    if (!g) return;
    g_mixer_count = (g->track_count < MAX_MIXER_TRACKS) ?
                    (int)g->track_count : MAX_MIXER_TRACKS;
    for (int i = 0; i < g_mixer_count; i++) {
        g_mixer_tracks[i].volume = g->tracks[i].volume;
        g_mixer_tracks[i].muted = g->tracks[i].muted;
        g_mixer_tracks[i].soloed = g->tracks[i].soloed;
    }
}

/* Set track volume */
void wb_audio_mixer_set_volume(int track, float vol) {
    if (track < 0 || track >= g_mixer_count) return;
    g_mixer_tracks[track].volume = vol < 0 ? 0 : (vol > 2 ? 2 : vol);
}

/* Set track pan */
void wb_audio_mixer_set_pan(int track, float pan) {
    if (track < 0 || track >= g_mixer_count) return;
    g_mixer_tracks[track].pan = pan < -1 ? -1 : (pan > 1 ? 1 : pan);
}

/* Set track mute */
void wb_audio_mixer_set_mute(int track, int mute) {
    if (track < 0 || track >= g_mixer_count) return;
    g_mixer_tracks[track].muted = mute;
}

/* Set track solo */
void wb_audio_mixer_set_solo(int track, int solo) {
    if (track < 0 || track >= g_mixer_count) return;
    g_mixer_tracks[track].soloed = solo;
}

/* Set master volume */
void wb_audio_mixer_set_master_volume(float vol) {
    g_master_volume = vol < 0 ? 0 : (vol > 2 ? 2 : vol);
}

/* Enable an input bus (Vegas 10 has 26) */
void wb_audio_mixer_enable_bus(int bus, int enable) {
    if (bus >= 0 && bus < MAX_INPUT_BUSES) {
        g_input_bus_enabled[bus] = enable;
    }
}

/* Get mixer track state */
const wb_mixer_track_t *wb_audio_mixer_get_track(int track) {
    if (track < 0 || track >= g_mixer_count) return NULL;
    return &g_mixer_tracks[track];
}

/* Get master volume */
float wb_audio_mixer_get_master_volume(void) {
    return g_master_volume;
}

/* Get number of mixer tracks */
int wb_audio_mixer_get_track_count(void) {
    return g_mixer_count;
}

/* Check if any track is soloed */
int wb_audio_mixer_any_soloed(void) {
    for (int i = 0; i < g_mixer_count; i++) {
        if (g_mixer_tracks[i].soloed) return 1;
    }
    return 0;
}

/* Apply mixer settings to audio samples before output.
 * buf: interleaved stereo float samples
 * n_frames: number of frames
 * track_idx: which track this buffer belongs to
 * Returns: pointer to processed buffer (same buffer, modified in place) */
float *wb_audio_mixer_process(float *buf, int n_frames, int track_idx) {
    if (!buf || track_idx < 0 || track_idx >= g_mixer_count) return buf;

    wb_mixer_track_t *mt = &g_mixer_tracks[track_idx];

    /* Check mute */
    if (mt->muted) {
        memset(buf, 0, n_frames * 2 * sizeof(float));
        return buf;
    }

    /* Check solo — if any track is soloed and this one isn't, mute */
    if (wb_audio_mixer_any_soloed() && !mt->soloed) {
        memset(buf, 0, n_frames * 2 * sizeof(float));
        return buf;
    }

    /* Apply volume and pan */
    float vol = mt->volume * g_master_volume;
    float left_gain = vol * (mt->pan <= 0 ? 1.0f : 1.0f - mt->pan);
    float right_gain = vol * (mt->pan >= 0 ? 1.0f : 1.0f + mt->pan);

    float peak = 0.0f;
    for (int i = 0; i < n_frames; i++) {
        buf[i * 2] *= left_gain;
        buf[i * 2 + 1] *= right_gain;

        /* Track peak for VU meter */
        float abs_l = fabsf(buf[i * 2]);
        float abs_r = fabsf(buf[i * 2 + 1]);
        if (abs_l > peak) peak = abs_l;
        if (abs_r > peak) peak = abs_r;
    }

    /* Update VU level (smoothed) */
    mt->vu_level = mt->vu_level * 0.9f + peak * 0.1f;
    if (peak > mt->peak_level) {
        mt->peak_level = peak;
    } else {
        mt->peak_level *= 0.99f;  /* decay */
    }

    return buf;
}

/* Reset peak hold */
void wb_audio_mixer_reset_peak(int track) {
    if (track >= 0 && track < g_mixer_count) {
        g_mixer_tracks[track].peak_level = 0.0f;
    }
}

/* Get VU level for a track (0..1) */
float wb_audio_mixer_get_vu(int track) {
    if (track < 0 || track >= g_mixer_count) return 0.0f;
    return g_mixer_tracks[track].vu_level;
}
