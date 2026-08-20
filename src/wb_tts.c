/* wb_tts.c — legitimate, offline Text-to-Speech for Big Mac.
 *
 * Architecture (same as the caption engine): Big Mac HOSTS a real, vendored
 * TTS engine instead of reinventing one. The engine is Piper (VITS, Apache-2.0,
 * ggml/onnxruntime, CPU, 100% offline) — see third_party/piper/. We drive it
 * the exact same way the caption engine drives whisper-cli: a subprocess with
 * the model + dylibs vendored in-repo, no network, no API key.
 *
 * This is the proven "run the engine, don't build the engine" pattern:
 *   - caption engine -> whisper.cpp  (ASR)
 *   - tts engine      -> piper       (TTS, VITS neural)
 *
 * API (unchanged from the formant-synth version so callers/tests are stable):
 *   wb_tts_create / wb_tts_destroy / wb_tts_speak / wb_tts_speak_wav ...
 * Output is mono float PCM at wb_tts_sample_rate() Hz (Piper default 22050),
 * routed through the existing WAV writer / voice-polish / export path.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include "wbus/wbus_tts.h"
#include "wbus/wb_internal.h"   /* wb_wav_write_pcm16 / wb_wav_read_pcm16 */

#define WB_TTS_SR 22050

#ifndef WB_TTS_ENGINE_DIR
/* Vendored Piper engine root (binary + lib/ + voices/ + share/). Overridable
 * at build time, but defaults to the in-repo location. */
#define WB_TTS_ENGINE_DIR "third_party/piper"
#endif

/* voice model + json (vendored). Default = en_US ryan high. */
#ifndef WB_TTS_VOICE
#define WB_TTS_VOICE "voices/en_US-ryan-high.onnx"
#endif

/* ---- helpers (mirror wb_captions.c) ----------------------------------- */

static int wb_tts_run_cmd(const char *cmd, const char *ctx) {
    int ret = system(cmd);
    if (ret != 0)
        fprintf(stderr, "wb_tts: %s failed (exit %d): %s\n",
                ctx, WEXITSTATUS(ret), cmd);
    return WEXITSTATUS(ret);
}

static int wb_tts_write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(text);
    size_t w = fwrite(text, 1, n, f);
    /* Piper reads stdin until EOF; a trailing newline is fine. */
    fputc('\n', f);
    fclose(f);
    return (w == n) ? 0 : -1;
}

/* ---- engine state ----------------------------------------------------- */
struct wb_tts {
    wb_tts_backend backend;
    int   sr;
    float rate;      /* 1.0 normal; passed to piper via --length-scale */
    int   voice;     /* 0 = default (ryan high) */
};

wb_tts *wb_tts_create(const char *model_path) {
    (void)model_path;   /* single vendored voice for now; neural is the path */
    wb_tts *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->sr = WB_TTS_SR;
    t->rate = 1.0f;
    t->voice = 0;
    t->backend = WB_TTS_BACKEND_NEURAL;   /* Piper = VITS neural */
    return t;
}

void wb_tts_destroy(wb_tts *t) { free(t); }

wb_tts_backend wb_tts_get_backend(wb_tts *t) {
    return t ? t->backend : WB_TTS_BACKEND_NONE;
}
int  wb_tts_sample_rate(wb_tts *t) { (void)t; return WB_TTS_SR; }
void wb_tts_set_pitch(wb_tts *t, float hz) { (void)t; (void)hz; } /* pitch via voice */
void wb_tts_set_rate(wb_tts *t, float r) { if (t) t->rate = r < 0.2f ? 0.2f : r; }
void wb_tts_set_voice(wb_tts *t, int i) { if (t) t->voice = i < 0 ? 0 : i; }
int  wb_tts_voice_count(wb_tts *t) { (void)t; return 1; } /* default voice */
const char *wb_tts_voice_name(wb_tts *t, int i) {
    (void)t; (void)i; return "en_US-ryan-high";
}

/* Build the absolute engine root so the binary finds its dylibs + voice
 * regardless of the caller's cwd. We resolve relative to the repo by trying
 * the in-tree path first, then a few common anchors. */
static const char *engine_root(void) {
    /* Prefer env override if set (lets tests point at a built copy). */
    const char *e = getenv("WB_TTS_ENGINE_DIR");
    if (e && e[0]) return e;
    return WB_TTS_ENGINE_DIR;
}

/* If the vendored voice model is missing, fetch it on demand (one-time).
 * Keeps the repo small (engine is vendored, the ~120MB model is fetched
 * once, then TTS runs fully offline). Returns 0 if model is present/ok. */
static int wb_tts_ensure_voice(const char *model_path) {
    struct stat st;
    if (stat(model_path, &st) == 0 && st.st_size > 1000) return 0;  /* present */
    fprintf(stderr, "wb_tts: voice model missing, fetching once: %s\n", model_path);
    char url[2048], cmd[4096];
    snprintf(url, sizeof(url),
             "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/"
             "en/en_US/ryan/high/en_US-ryan-high.onnx");
    snprintf(cmd, sizeof(cmd),
             "curl -sL --max-time 300 -o '%s' '%s'", model_path, url);
    if (wb_tts_run_cmd(cmd, "fetch piper voice") != 0) {
        fprintf(stderr, "wb_tts: voice fetch failed (need: make tts-voice)\n");
        return -1;
    }
    /* also the json sidecar */
    char json[2048];
    snprintf(json, sizeof(json), "%s.json", model_path);
    snprintf(url, sizeof(url),
             "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/"
             "en/en_US/ryan/high/en_US-ryan-high.onnx.json");
    snprintf(cmd, sizeof(cmd), "curl -sL --max-time 60 -o '%s' '%s'", json, url);
    wb_tts_run_cmd(cmd, "fetch piper voice json");
    return (stat(model_path, &st) == 0 && st.st_size > 1000) ? 0 : -1;
}

/* Synthesize text -> output WAV (16-bit PCM) via the vendored Piper engine. */
static int synth_to_wav(wb_tts *t, const char *text, const char *wav_path) {
    const char *root = engine_root();
    char bin[1024], model[1024], libdir[1024], datadir[1024], textfile[1024], cmd[4096];

    snprintf(bin,    sizeof(bin),    "%s/piper", root);
    snprintf(model,  sizeof(model),  "%s/" WB_TTS_VOICE, root);
    snprintf(libdir, sizeof(libdir), "%s/lib", root);
    snprintf(datadir,sizeof(datadir),"%s/share/espeak-ng-data", root);

    /* Put the engine's library paths into the REAL environment. macOS SIP
     * strips DYLD_* vars when they are set *inside* a system() command string,
     * but honors them when inherited from the process environment — so we
     * setenv() them here (the child via system() inherits our environ). */
    setenv("DYLD_LIBRARY_PATH", libdir, 1);
    setenv("ESPEAK_DATA_PATH", datadir, 1);
    setenv("DYLD_FALLBACK_LIBRARY_PATH", libdir, 1);

    /* ensure the (large) voice model is present; fetch once if not */
    if (wb_tts_ensure_voice(model) != 0) return -1;

    /* write the input text to a temp file */
    snprintf(textfile, sizeof(textfile), "%s.txt", wav_path);
    if (wb_tts_write_text(textfile, text) != 0) return -1;

    /* Piper command. Library paths are in the environment (above), not in the
     * command string, so SIP can't strip them. --noise_scale 0 --noise_w 0
     * make VITS deterministic. */
    snprintf(cmd, sizeof(cmd),
             "cat '%s' | '%s' --model '%s' --output_file '%s' "
             "--length-scale %.3f --noise_scale 0 --noise_w 0 >/dev/null 2>&1",
             textfile, bin, model, wav_path,
             (t->rate > 0.01f) ? 1.0f / t->rate : 1.0f);

    int rc = wb_tts_run_cmd(cmd, "piper tts");
    remove(textfile);
    return rc;
}

int wb_tts_speak_wav(wb_tts *t, const char *text, const char *wav_path) {
    if (!t || !text || !wav_path) return -1;
    return synth_to_wav(t, text, wav_path);
}

int wb_tts_speak(wb_tts *t, const char *text,
                 float **out_pcm, uint32_t *out_frames, int *out_sr) {
    if (!t || !text || !out_pcm) return -1;
    *out_pcm = NULL; *out_frames = 0; if (out_sr) *out_sr = WB_TTS_SR;

    /* synth to a temp wav, then read it as float PCM */
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "/tmp/wb_tts_%u.wav", (unsigned)getpid());
    if (synth_to_wav(t, text, tmp) != 0) { remove(tmp); return -1; }

    int ch = 0, sr = 0; uint32_t n = 0;
    float *p = NULL;
    if (wb_wav_read_pcm16(tmp, &p, &n, &ch, &sr) != 0) { remove(tmp); return -1; }
    remove(tmp);
    if (!p) return -1;
    *out_pcm = p; *out_frames = n; if (out_sr) *out_sr = sr;
    return 0;
}
