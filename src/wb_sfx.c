/* wb_sfx.c — R074 hop 150 (G-SF070): procedural game-SFX synthesizer.
 * Laser (descending FM zap), explosion (filtered noise burst), pickup
 * (rising arpeggio blip), powerup (sweep). Pure C11, no samples.
 *
 * API: render into a caller buffer at any sample rate. One-shot voices,
 * stateless presets — deterministic under the G-SF097 float policy.
 */

#include "wbus/wbus_sfx.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TWO_PI 6.2831853071795864769

/* laser: square-ish carrier sweeping f0 -> f1 over dur, sharp env */
int wb_sfx_laser(float *out, int n, uint32_t sr,
                 float f0, float f1, float dur) {
    if (!out || n <= 0 || sr == 0 || dur <= 0) return -1;
    int len = (int)(dur * sr); if (len > n) len = n;
    double ph = 0.0;
    for (int i = 0; i < len; i++) {
        double t = (double)i / len;
        float f = f0 + (f1 - f0) * (float)t;
        ph += TWO_PI * f / sr;
        if (ph >= TWO_PI) ph -= TWO_PI;
        /* band-limited-ish square via sign of sine */
        float sq = sin(ph) >= 0.0 ? 0.6f : -0.6f;
        float env = (float)pow(1.0 - t, 2.0);
        out[i] = sq * env;
    }
    for (int i = len; i < n; i++) out[i] = 0.0f;
    return len;
}

/* explosion: brown-noise burst with exponential decay + low rumble */
int wb_sfx_explosion(float *out, int n, uint32_t sr, float dur) {
    if (!out || n <= 0 || sr == 0 || dur <= 0) return -1;
    int len = (int)(dur * sr); if (len > n) len = n;
    uint32_t rng = 0x1234567u;   /* fixed seed: reproducible */
    double lp = 0.0, rumble_ph = 0.0;
    for (int i = 0; i < len; i++) {
        double t = (double)i / len;
        /* xorshift noise */
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        float white = ((float)(int32_t)rng / 2147483648.0f);
        lp += 0.06 * (white - lp);              /* one-pole LP -> brown */
        rumble_ph += TWO_PI * (55.0 - 30.0 * t) / sr;
        float env = (float)exp(-4.5 * t);
        out[i] = (float)lp * 2.2f * env
               + 0.25f * (float)sin(rumble_ph) * env;
    }
    for (int i = len; i < n; i++) out[i] = 0.0f;
    return len;
}

/* pickup: quick rising two-note blip (coin style) */
int wb_sfx_pickup(float *out, int n, uint32_t sr) {
    if (!out || n <= 0 || sr == 0) return -1;
    const float notes[2] = { 987.77f, 1318.51f };  /* B5, E6 */
    const float note_dur = 0.075f;
    int per = (int)(note_dur * sr);
    int pos = 0;
    for (int v = 0; v < 2; v++) {
        for (int i = 0; i < per && pos < n; i++, pos++) {
            double t = (double)i / per;
            float env = 1.0f - (float)t * 0.4f;
            out[pos] = 0.35f * (float)sin(TWO_PI * notes[v] * i / sr)
                     * env;
        }
    }
    for (; pos < n; pos++) out[pos] = 0.0f;
    return pos;
}

/* powerup: exponential frequency sweep up with vibrato */
int wb_sfx_powerup(float *out, int n, uint32_t sr, float dur) {
    if (!out || n <= 0 || sr == 0 || dur <= 0) return -1;
    int len = (int)(dur * sr); if (len > n) len = n;
    double ph = 0.0;
    for (int i = 0; i < len; i++) {
        double t = (double)i / len;
        float f = 220.0f * (float)pow(2.0, 3.0 * t);   /* 220 -> ~1760 */
        f += 12.0f * (float)sin(TWO_PI * 9.0 * i / sr); /* vibrato */
        ph += TWO_PI * f / sr;
        if (ph >= TWO_PI) ph -= TWO_PI;
        float env = 0.4f * (float)(0.7 + 0.3 * sin(t * 3.14159));
        out[i] = 0.45f * (float)sin(ph) * env;
    }
    for (int i = len; i < n; i++) out[i] = 0.0f;
    return len;
}
