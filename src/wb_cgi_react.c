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
