/* mk_tts_podcast.c — produce a podcast episode ENTIRELY with Big Mac's own
 * code (no external/online TTS). Pipeline:
 *   1. wb_tts (R019, pure-C11 formant synth) -> raw WAV
 *   2. wb_voice_polish_apply_twopass (G8)   -> normalize to -16 LUFS
 * Then (optionally, via ffmpeg/whisper outside this tool) the WAV can be
 * transcribed and captioned. This tool proves the engine is self-sufficient.
 *
 * Usage: mk_tts_podcast "<script text>" out.wav */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus_tts.h"
#include "wbus/wbus_voice_polish.h"
#include "wbus/wb_internal.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s \"<text>\" out.wav\n", argv[0]); return 2; }
    const char *text = argv[1];
    const char *out  = argv[2];

    /* 1. synthesize with the in-repo TTS engine */
    wb_tts *t = wb_tts_create(NULL);  /* NULL model -> phonetic backend (offline) */
    if (!t) { fprintf(stderr, "tts create failed\n"); return 1; }
    wb_tts_set_voice(t, 0);      /* default voice */
    wb_tts_set_pitch(t, 1.0f);    /* neutral */
    char raw[512];
    snprintf(raw, sizeof(raw), "%s.raw.wav", out);
    if (wb_tts_speak_wav(t, text, raw) != 0) {
        fprintf(stderr, "tts speak failed\n"); wb_tts_destroy(t); return 1;
    }
    wb_tts_destroy(t);

    /* 2. read, two-pass polish to -16 LUFS, write */
    float *buf = NULL; int ch = 0, sr = 0; uint32_t n = 0;
    if (wb_wav_read_pcm16(raw, &buf, &n, &ch, &sr) != 0) {
        fprintf(stderr, "read %s failed\n", raw); return 1;
    }
    float before = wb_loudness_measure(buf, n, ch, (float)sr);
    wb_voice_polish_apply_twopass(buf, n, ch, (float)sr, -16.0f);
    float after = wb_loudness_measure(buf, n, ch, (float)sr);
    if (wb_wav_write_pcm16(out, buf, n, ch, sr) != 0) {
        fprintf(stderr, "write %s failed\n", out); free(buf); return 1;
    }
    printf("OFFLINE podcast: '%s' -> %s\n", out, raw);
    printf("  loudness: %.1f LUFS -> %.1f LUFS (target -16)\n", before, after);
    printf("  %u frames, %d ch, %d Hz, %.2fs\n", n, ch, sr, (float)n/sr);
    free(buf);
    return 0;
}
