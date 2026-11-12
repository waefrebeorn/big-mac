/*
 * bigmac_out.h -- the host audio output backend for WuBuOS.
 *
 * WuBuOS's audio engine (src/audio/) renders into a caller-provided float
 * buffer. Nothing ever called it: there was no device path at all. This is
 * the missing back half -- it takes those frames and puts them on a real
 * speaker.
 *
 * One file, three backends, in order of preference:
 *
 *   pulse  PulseAudio (pa_simple). What a desktop Linux box actually runs;
 *          goes through PipeWire transparently.
 *   alsa   snd_pcm_writei on "default". What a bare-metal install has.
 *   wav    write a 16-bit PCM WAV to a file. What a headless CI box has --
 *          it still proves the pipeline end to end instead of silently
 *          producing nothing.
 *
 * The choice is made once at init and reported by bigmac_out_backend_name()
 * so the caller can log what it got. Failure of one backend falls through to
 * the next; only total failure returns an error.
 */
#ifndef BIGMAC_OUT_H
#define BIGMAC_OUT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which backend actually opened. */
typedef enum {
    BIGMAC_BACKEND_NONE = 0,
    BIGMAC_BACKEND_PULSE,
    BIGMAC_BACKEND_ALSA,
    BIGMAC_BACKEND_WAV
} BigmacBackend;

/* Open the first backend that works.
 * sample_rate: 44100/48000/...
 * channels:    1 or 2
 * wav_path:    where the wav backend writes (ignored otherwise).
 * Returns 0 on success, -1 if no backend could open. */
int  bigmac_out_init(int sample_rate, int channels, const char *wav_path);

/* Push interleaved float frames (range -1..1) to the device.
 * Returns the number of frames accepted, or -1 on a hard error.
 * Never blocks longer than the device's own buffering. */
long bigmac_out_write(const float *frames, int nframes);

/* Frames the backend has accepted but not yet played (buffer depth). */
int  bigmac_out_latency_frames(void);

/* What actually opened. */
BigmacBackend bigmac_out_backend(void);
const char   *bigmac_out_backend_name(void);

/* Drain and close. Safe to call when nothing was opened. */
void bigmac_out_close(void);

#ifdef __cplusplus
}
#endif

#endif /* BIGMAC_OUT_H */
