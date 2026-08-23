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

/* G55: named loudness profiles (spec target LUFS + true-peak ceiling dBTP).
 * Sources: EBU R128 (-23/-1), ATSC A/85 CALM Act (-24/-2),
 * Netflix Partner Help v1.6 (-27 dialogue-gated, -2 dBTP ceiling here),
 * YouTube streaming (-14/-1.5), podcast -16. */
typedef struct {
    const char *name;
    double lufs;        /* integrated target */
    double tp_ceiling;  /* true-peak max dBTP (negative) */
    double lra_max;     /* loudness-range cap for loudnorm (0=engine 11) */
} wb_delivery_profile;

const wb_delivery_profile *wb_delivery_profiles(int *count_out);
const wb_delivery_profile *wb_delivery_profile_by_name(const char *name);

/* Two-pass loudnorm measurement of a WAV file.
 * Writes measured_I/tp/LRA/thresh (needed by pass 2). Returns 0. */
int wb_delivery_measure_loudness(const char *wav_path,
                                 double *i_out, double *tp_out,
                                 double *lra_out, double *thresh_out);

/* Normalize a WAV in place (pass 2) to target LUFS with linear mode. */
int wb_delivery_normalize_wav(const char *wav_path, double target_lufs);

/* G41 (Wave2): stems export — render every non-bus track to its own WAV
 * (zero-based start, full session length) into dir/trackNN_name.wav.
 * Returns the number of stems written, or -1 on error. */
int wb_delivery_export_stems(wb_session *s, const char *dir);

/* Generate YouTube description chapter block from session markers into buf:
 * "00:00 Intro\n00:12 Verse\n..." (requires >=2 markers; first at 0).
 * Returns number of chapters written, or 0 if markers insufficient. */
int wb_delivery_chapters(const wb_session *s, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_DELIVERY_H */
