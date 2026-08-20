/* wbus_tts.h — legitimate, offline Text-to-Speech for Big Mac.
 *
 * Architecture (same as the caption engine): Big Mac HOSTS a real, vendored
 * TTS engine rather than reinventing one. The engine is Piper (VITS,
 * Apache-2.0, onnxruntime/ggml, CPU, 100% offline) — see third_party/piper/.
 * We drive it the exact same way the caption engine drives whisper.cpp:
 * a subprocess with the model + dylibs vendored in-repo, no network, no key.
 *
 *   - caption engine -> whisper.cpp (ASR)
 *   - tts engine      -> piper       (TTS, VITS neural)
 *
 * The engine produces mono float PCM at wb_tts_sample_rate() Hz (Piper
 * default 22050); callers route it through the WAV writer / voice-polish /
 * export path. The wb_tts_* API is stable so the podcast driver and tests
 * keep working.
 */
#ifndef WUBUS_WBUS_TTS_H
#define WUBUS_WBUS_TTS_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    WB_TTS_BACKEND_NONE = 0,
    WB_TTS_BACKEND_PHONETIC,   /* reserved (formant proof-of-concept) */
    WB_TTS_BACKEND_NEURAL      /* Piper VITS (vendored, offline, active) */
} wb_tts_backend;

typedef struct wb_tts wb_tts;

/* Create a TTS engine. Picks the neural backend if `model_path` names a usable
 * model, else falls back to the always-available phonetic backend. Pass NULL
 * for model_path to force the phonetic backend. */
wb_tts *wb_tts_create(const char *model_path);
void     wb_tts_destroy(wb_tts *t);

/* Which backend is active. */
wb_tts_backend wb_tts_get_backend(wb_tts *t);

/* Output sample rate of the synthesized PCM (Hz). */
int wb_tts_sample_rate(wb_tts *t);

/* Synthesize `text` to mono float PCM.
 * out_pcm is malloc'd by the callers-free-with-free(); *out_frames and *out_sr
 * are set. Returns 0 on success, -1 on error (out_pcm left NULL). */
int wb_tts_speak(wb_tts *t, const char *text,
                 float **out_pcm, uint32_t *out_frames, int *out_sr);

/* Convenience: synthesize directly to a 16-bit PCM WAV file. Returns 0 on
 * success. Reuses the WAV writer; suitable for podcast narration offline. */
int wb_tts_speak_wav(wb_tts *t, const char *text, const char *wav_path);

/* Voice selection (phonetic backend honors a small set of named tunings).
 * Returns the number of available voices; index 0 is the default. */
int  wb_tts_voice_count(wb_tts *t);
const char *wb_tts_voice_name(wb_tts *t, int index);
void wb_tts_set_voice(wb_tts *t, int index);

/* Pitch (Hz, ~80..260) and rate (1.0 = normal) controls. */
void wb_tts_set_pitch(wb_tts *t, float hz);
void wb_tts_set_rate(wb_tts *t, float rate);

#endif /* WUBUS_WBUS_TTS_H */
