/* wbus_delivery.h — export delivery presets (R064).
 *
 * One call to produce a platform-ready master: applies the two-pass
 * EBU R128 loudness normalization to the session's engine-rendered audio,
 * muxes with video, tags faststart for streaming, and emits a
 * YouTube-chapters text block from arrangement markers.
 *
 * Presets:
 *   YOUTUBE  - target -14 LUFS, true peak -1.5 dBTP, H.264 crf20,
 *              +faststart, Rec709, chapter list printed
 *   PODCAST  - audio-only -16 LUFS stereo (mp3 via ffmpeg)
 *
 * C11; ffmpeg CLI only.
 */
#ifndef WUBUS_WBUS_DELIVERY_H
#define WUBUS_WBUS_DELIVERY_H

#include <stdint.h>
#include <stddef.h>
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WB_DELIVERY_YOUTUBE = 0,
    WB_DELIVERY_PODCAST
} wb_delivery_preset;

/* Two-pass loudnorm measurement of a WAV file.
 * Writes measured_I/tp/LRA/thresh (needed by pass 2). Returns 0. */
int wb_delivery_measure_loudness(const char *wav_path,
                                 double *i_out, double *tp_out,
                                 double *lra_out, double *thresh_out);

/* Normalize a WAV in place (pass 2) to target LUFS with linear mode. */
int wb_delivery_normalize_wav(const char *wav_path, double target_lufs);

/* Generate YouTube description chapter block from session markers into buf:
 * "00:00 Intro\n00:12 Verse\n..." (requires >=2 markers; first at 0).
 * Returns number of chapters written, or 0 if markers insufficient. */
int wb_delivery_chapters(const wb_session *s, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_DELIVERY_H */
