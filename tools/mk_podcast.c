/* mk_podcast.c — Big Mac pipeline driver for a TTS podcast episode.
 * Reads a raw WAV (mono/stereo), runs it through wb_voice_polish_apply_twopass
 * (G8 two-pass BS.1770 loudness norm to -16 LUFS, gate->deess->comp->EQ->limiter),
 * writes the polished WAV. Pure C11, uses the real Big Mac voice-polish chain. */
#include <stdio.h>
#include <stdlib.h>
#include "wbus/wbus_voice_polish.h"
#include "wbus/wb_internal.h"

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <in.wav> <out.wav> <lufs>\n", argv[0]); return 1; }
    const char *in = argv[1], *out = argv[2];
    float lufs = (float)atof(argv[3]);

    float *data = NULL; uint32_t frames = 0; int ch = 0, sr = 0;
    if (wb_wav_read_pcm16(in, &data, &frames, &ch, &sr) != 0 || !data) {
        fprintf(stderr, "read failed: %s\n", in); return 1;
    }
    fprintf(stderr, "[mk_podcast] loaded %s: %u frames, %d ch, %d sr\n",
            in, frames, ch, sr);

    float before = wb_loudness_measure(data, frames, ch, (float)sr);
    fprintf(stderr, "[mk_podcast] loudness before = %.1f LUFS\n", before);

    if (wb_voice_polish_apply_twopass(data, frames, ch, (float)sr, lufs) != 0) {
        fprintf(stderr, "polish failed\n"); return 1;
    }

    float after = wb_loudness_measure(data, frames, ch, (float)sr);
    fprintf(stderr, "[mk_podcast] loudness after  = %.1f LUFS (target %.1f)\n", after, lufs);

    if (wb_wav_write_pcm16(out, data, frames, ch, sr) != 0) {
        fprintf(stderr, "write failed: %s\n", out); return 1;
    }
    fprintf(stderr, "[mk_podcast] wrote %s\n", out);
    free(data);
    return 0;
}
