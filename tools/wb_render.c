/* wb_render.c — CLI offline render: build/load a session, render the whole
 * song to a WAV file through the exact same engine path as live playback.
 *
 * R073 hop 35/36: reports BS.1770 integrated LUFS + peak dBFS after render;
 * --lufs N trims the export to N LUFS (true-peak guarded) before writing.
 *
 * Usage: wb_render [out.wav] [--demo|--file demo.wbus] [--lufs N]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wbus.h"
#include "wb_internal.h"
#include "wbus/wbus_lufs.h"

int main(int argc, char **argv) {
    const char *outpath = argc > 1 ? argv[1] : "render.wav";
    double lufs_target = 0.0;   /* 0 = off; --lufs N enables */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--lufs") == 0 && i + 1 < argc)
            lufs_target = atof(argv[++i]);
    wb_session *s = NULL;

    if (argc > 2 && strcmp(argv[2], "--demo") == 0) {
        s = wb_session_demo();
        printf("Rendering demo session (%d tracks, %.1fs)\n",
               (int)s->track_count, s->length / WB_SAMPLE_RATE);
    } else if (argc > 2 && strcmp(argv[2], "--file") == 0 && argc > 3) {
        s = wb_session_load(argv[3]);
        if (!s) { fprintf(stderr, "render: failed to load %s\n", argv[3]); return 1; }
        printf("Rendering project %s (%d tracks, %.1fs)\n",
               argv[3], (int)s->track_count, s->length / WB_SAMPLE_RATE);
    } else {
        fprintf(stderr,
            "usage: wb_render out.wav --demo | --file project.wbus [--lufs N]\n");
        return 2;
    }

    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);

    wb_sample *audio = NULL;
    uint32_t frames = 0;
    if (wb_engine_render_session(e, s, &audio, &frames) != 0) {
        fprintf(stderr, "render failed\n");
        return 1;
    }

    /* measure sample peak + RMS */
    float peak = 0, rms_acc = 0;
    for (uint32_t i = 0; i < frames * 2; i++) {
        float v = audio[i];
        float a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        rms_acc += v * v;
    }
    float rms = sqrtf(rms_acc / (frames * 2));

    /* R073 hop 35: BS.1770 integrated loudness via the engine's own
     * K-weighting meter */
    float *mono = malloc((size_t)frames * sizeof(float));
    double lufs_i = 0.0;
    if (mono) {
        for (uint32_t i = 0; i < frames; i++)
            mono[i] = 0.5f * (audio[i*2] + audio[i*2+1]);
        wb_lufs lv;
        wb_lufs_create(&lv, WB_SAMPLE_RATE);
        wb_lufs_process(&lv, mono, (int)frames);
        lufs_i = wb_lufs_integrated_lufs(&lv);
    }

    printf("Rendered %u frames (%.2fs) -> %s\n", frames,
           (double)frames / WB_SAMPLE_RATE, outpath);
    printf("Peak: %.3f  RMS: %.3f\n", peak, rms);
    double pk_db = 20.0 * log10(peak > 1e-6 ? peak : 1e-6);
    printf("LUFS-I: %.1f   Sample-peak: %.2f dBFS\n", lufs_i, pk_db);

    /* R073 hop 36: optional loudness trim before writing */
    if (lufs_target < 0.0 && lufs_i > -70.0) {
        double g_db = lufs_target - lufs_i;
        if (peak > 1e-6f) {
            double cap = 20.0 * log10(1.0 / peak);
            if (g_db > cap) g_db = cap;      /* true-peak guard */
        }
        float g = (float)pow(10.0, g_db / 20.0);
        for (uint32_t i = 0; i < frames * 2; i++) audio[i] *= g;
        rms *= g; peak *= g;
        printf("LUFS trim: %.1f -> %.1f LUFS (%+.1f dB)\n",
               lufs_i, lufs_target, g_db);
    }
    free(mono);

    if (wb_wav_write_pcm16(outpath, audio, frames, 2, WB_SAMPLE_RATE) != 0) {
        fprintf(stderr, "wav write failed\n");
        return 1;
    }
    printf("Final: Peak %.3f (%.2f dBFS)  RMS %.3f\n",
           peak, 20.0 * log10(peak > 1e-6 ? peak : 1e-6), rms);

    free(audio);
    wb_engine_destroy(e);
    wb_session_destroy(s);
    return 0;
}
