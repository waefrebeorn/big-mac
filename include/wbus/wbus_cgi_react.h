/* wb_cgi_react.h — R073 hop 55: audio-reactive CGI animation. */
#ifndef WBUS_CGI_REACT_H
#define WBUS_CGI_REACT_H

#include "wbus.h"

struct wb_anim;
/* Bake an audio RMS envelope into scale keys on an anim object:
 * scale = base + amount * normalized_rms per window (16 windows). */
int wb_cgi_audio_pulse(struct wb_anim *a, int obj,
                       const wb_sample *audio, uint32_t frames,
                       uint32_t chn, double dur_secs,
                       float base, float amount);

struct wb_session;
/* R073 hop 56: beat-aligned pulse — keys land on the estimated beat grid;
 * scale follows per-beat onset loudness. */
int wb_cgi_beat_pulse(struct wb_session *s, int track, int clip,
                      struct wb_anim *a, int obj,
                      float base, float amount);
#endif
