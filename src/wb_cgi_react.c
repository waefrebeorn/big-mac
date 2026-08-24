/* wb_cgi_react.c — R073 hop 55: audio-reactive CGI animation.
 *
 * Bridges the audio and 3D layers: samples an audio buffer's RMS envelope
 * on a grid and writes scale keyframes onto a wb_anim object, so a 3D
 * object pulses with the music (the classic visualizer workflow, done
 * offline like Blender's "bake to f-curves"). Pure arithmetic; C11.
 */
#include <stdlib.h>
#include <math.h>
#include "wbus/wbus.h"
#include "wbus/wbus_anim.h"

/* Analyze `audio` (interleaved `chn`) in `beats` windows across `dur_secs`
 * and write scale keys: scale = base + amount * rms_normalized. Keys land
 * at each window center. Returns the number of keys written, or -1. */
int wb_cgi_audio_pulse(wb_anim *a, int obj,
                       const wb_sample *audio, uint32_t frames,
                       uint32_t chn, double dur_secs,
                       float base, float amount) {
    if (!a || !audio || frames == 0 || chn == 0 || dur_secs <= 0)
        return -1;
    const int beats = 16;                       /* resolution of the pulse */
    uint32_t win = frames / beats;
    if (win < 1) return -1;

    /* first pass: RMS per window + global max */
    float rms[16]; float gmax = 1e-6f;
    for (int b = 0; b < beats; b++) {
        double sum = 0;
        uint32_t s0 = (uint32_t)((double)b * frames / beats);
        uint32_t s1 = s0 + win;
        if (s1 > frames) s1 = frames;
        for (uint32_t i = s0; i < s1; i++) {
            for (uint32_t c = 0; c < chn; c++) {
                double v = audio[i * chn + c];
                sum += v * v;
            }
        }
        rms[b] = sqrtf((float)(sum / (double)((s1 - s0) * chn)));
        if (rms[b] > gmax) gmax = rms[b];
    }

    /* second pass: bake normalized envelope into scale keys */
    int wrote = 0;
    for (int b = 0; b < beats; b++) {
        double t = ((double)b + 0.5) * dur_secs / beats;
        float norm = rms[b] / gmax;
        float scale = base + amount * norm;
        if (wb_anim_key(a, obj, t,
                        0, 0, -2,          /* default camera-facing pos */
                        0.0f, norm * 6.283f, 0.0f,
                        scale) == 0)
            wrote++;
    }
    return wrote;
}

/* ---- R073 hop 56: beat-grid pulse ------------------------------------------ */
/* Like wb_cgi_audio_pulse, but keys land on the BEAT GRID (from
 * wb_session_estimate_bpm) and scale follows per-beat onset strength, so
 * pulses are musically aligned. Needs a session for tempo estimation.
 * Returns keys written or -1. */
int wb_cgi_beat_pulse(wb_session *s, int track, int clip,
                      wb_anim *a, int obj,
                      float base, float amount);

int wb_cgi_beat_pulse(wb_session *s, int track, int clip,
                      wb_anim *a, int obj,
                      float base, float amount) {
    if (!s || !a) return -1;
    double bpm = wb_session_estimate_bpm(s, track, clip);
    if (bpm < 30.0) return -1;                   /* unsure */
    const wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip >= tr->clip_count) return -1;
    const wb_clip *cl = &tr->clips[clip];
    if (!cl->audio_data || cl->audio_frames == 0) return -1;

    /* per-beat onset loudness: RMS in the first quarter of each beat */
    double beat_sec = 60.0 / bpm;
    int nbeats = (int)(cl->audio_frames / WB_SAMPLE_RATE / beat_sec);
    if (nbeats < 2) return -1;
    uint32_t ch = cl->audio_channels > 0 ? cl->audio_channels : 1;

    float rms[512]; float gmax = 1e-6f;
    if (nbeats > 512) nbeats = 512;
    for (int b = 0; b < nbeats; b++) {
        uint32_t s0 = (uint32_t)(b * beat_sec * WB_SAMPLE_RATE);
        uint32_t s1 = s0 + (uint32_t)(beat_sec * WB_SAMPLE_RATE * 0.25);
        if (s1 > cl->audio_frames) s1 = cl->audio_frames;
        double sum = 0; uint32_t cnt = 0;
        for (uint32_t i = s0; i < s1; i++) {
            for (uint32_t c2 = 0; c2 < ch; c2++) {
                double v = cl->audio_data[i * ch + c2];
                sum += v * v; cnt++;
            }
        }
        rms[b] = cnt ? sqrtf((float)(sum / cnt)) : 0.0f;
        if (rms[b] > gmax) gmax = rms[b];
    }

    int wrote = 0;
    for (int b = 0; b < nbeats; b++) {
        double t = b * beat_sec;
        float norm = rms[b] / gmax;
        float scale = base + amount * norm;
        if (wb_anim_key(a, obj, t,
                        0, 0, -2, 0.0f, norm * 3.14159f, 0.0f,
                        scale) == 0)
            wrote++;
    }
    return wrote;
}

/* ---- R073 hop 57: camera shake from transients ------------------------------ */
/* Add camera-shake keys: at each detected transient the camera kicks by a
 * pseudo-random angle offset that decays over ~200ms (keys at hit + decay
 * steps). Uses G27 detector positions. Returns keys written or -1. */
int wb_cgi_camera_shake(wb_session *s, int track, int clip,
                        wb_anim *a, float intensity) {
    if (!s || !a || intensity <= 0.0f) return -1;
    uint32_t hits[256];
    int nh = wb_session_detect_transients(s, track, clip, 0.5f,
                                          hits, 256);
    if (nh <= 0) return -1;
    const double DECAY = 0.2;                    /* seconds per decay */
    int wrote = 0;
    unsigned seed = 12345;
    for (int k = 0; k < nh; k++) {
        double t0 = (double)hits[k] / WB_SAMPLE_RATE;
        /* kick + a few decaying wobble keys */
        for (int step = 0; step < 4; step++) {
            double t = t0 + step * (DECAY / 4);
            seed = seed * 1103515245 + 12345;
            float sgn = ((seed >> 16) & 1) ? 1.0f : -1.0f;
            float amp = intensity * sgn / (float)(step + 1);
            if (wb_anim_key_camera(a, t,
                                   amp * 0.3f,     /* pitch kick */
                                   amp * 0.5f,     /* yaw kick   */
                                   5.0f + amp) == 0)
                wrote++;
        }
    }
    return wrote;
}
