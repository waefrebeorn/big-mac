/* wb_backend.c — audio backend: SDL2 audio device (realtime) and WAV file
 * (offline). Both pull from the exact same engine render path (oracle
 * parity): what you hear is what you export.
 *
 * SDL2 audio gives us a portable realtime device on macOS (CoreAudio),
 * Linux (PulseAudio/ALSA), and Windows (WASAPI). No platform-specific code.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <SDL.h>

#include "wbus.h"
#include "wbus_backend.h"

struct wb_backend {
    wb_backend_info info;
    int kind;                 /* 0 = sdl audio, 1 = file */
    wb_engine *engine;
    /* SDL audio state */
    SDL_AudioDeviceID device;
    /* file backend state */
    FILE *file;
    uint32_t file_frames_written;
    uint32_t file_total_frames;
    /* pull buffer for the callback */
    wb_sample pull[WB_MAX_BLOCK * 2];
};

static int finalize_wav(wb_backend *b);

/* ---- SDL audio callback (realtime thread) ----------------------------- */
static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
    wb_backend *b = userdata;
    int frames = len / (sizeof(Sint16) * 2);   /* 16-bit stereo */
    if (frames > (int)WB_MAX_BLOCK) frames = WB_MAX_BLOCK;
    uint32_t got = wb_engine_render(b->engine, b->pull, (uint32_t)frames);
    Sint16 *out = (Sint16 *)stream;
    for (int i = 0; i < frames; i++) {
        float L = (i < (int)got) ? b->pull[2*i] : 0.0f;
        float R = (i < (int)got) ? b->pull[2*i+1] : 0.0f;
        if (L > 1.0f) L = 1.0f; else if (L < -1.0f) L = -1.0f;
        if (R > 1.0f) R = 1.0f; else if (R < -1.0f) R = -1.0f;
        out[2*i]   = (Sint16)(L * 32767.0f);
        out[2*i+1] = (Sint16)(R * 32767.0f);
    }
}

/* ---- create ----------------------------------------------------------- */
wb_backend *wb_backend_coreaudio_create(wb_engine *e, uint32_t sr) {
    (void)sr;
    wb_backend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->kind = 0;
    b->engine = e;
    b->info.sample_rate = 44100;
    b->info.channels = 2;
    snprintf(b->info.name, sizeof(b->info.name), "SDL2 Audio");

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = sdl_audio_callback;
    want.userdata = b;

    b->device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (b->device == 0) {
        fprintf(stderr, "wb_backend: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        free(b);
        return NULL;
    }
    return b;
}

wb_backend *wb_backend_file_create(wb_engine *e, const char *path, uint32_t sr) {
    wb_backend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->kind = 1;
    b->engine = e;
    b->info.sample_rate = sr;
    b->info.channels = 2;
    snprintf(b->info.name, sizeof(b->info.name), "WAV");
    b->file = fopen(path, "wb");
    if (!b->file) { free(b); return NULL; }
    return b;
}

const wb_backend_info *wb_backend_get_info(const wb_backend *b) { return b ? &b->info : NULL; }

/* ---- start ------------------------------------------------------------ */
int wb_backend_start(wb_backend *b) {
    if (!b) return -1;
    if (b->kind == 1) {
        /* offline: pull until session end */
        wb_session *s = wb_engine_get_session(b->engine);
        double length = s ? s->length : 0;
        b->file_total_frames = length > 0 ? (uint32_t)length : (uint32_t)(10 * b->info.sample_rate);
        wb_sample buf[WB_MAX_BLOCK * 2];
        uint32_t done = 0;
        while (done < b->file_total_frames) {
            uint32_t n = (b->file_total_frames - done);
            if (n > WB_MAX_BLOCK) n = WB_MAX_BLOCK;
            uint32_t got = wb_engine_render(b->engine, buf, n);
            if (got == 0) break;
            fwrite(buf, sizeof(wb_sample), got * 2, b->file);
            b->file_frames_written += got;
            done += got;
        }
        /* rewrite the WAV header with actual lengths */
        return finalize_wav(b);
    }
    /* realtime: start SDL audio device (callback pulls from engine) */
    SDL_PauseAudioDevice(b->device, 0);
    return 0;
}

/* Write a minimal WAV header into the stream; called after the file is
 * complete so the data length is correct. */
static int finalize_wav(wb_backend *b) {
    long pos = ftell(b->file);
    uint32_t data_bytes = b->file_frames_written * 2 * 2;
    uint32_t riff_size = 36 + data_bytes;

    fseek(b->file, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, b->file);
    fwrite(&riff_size, 4, 1, b->file);
    fwrite("WAVE", 1, 4, b->file);
    fwrite("fmt ", 1, 4, b->file);
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1, ch = 2, bits = 16;
    uint32_t sample_rate = b->info.sample_rate;
    uint32_t byte_rate = sample_rate * ch * 2;
    uint16_t block_align = ch * 2;
    fwrite(&fmt_size, 4, 1, b->file);
    fwrite(&audio_format, 2, 1, b->file);
    fwrite(&ch, 2, 1, b->file);
    fwrite(&sample_rate, 4, 1, b->file);
    fwrite(&byte_rate, 4, 1, b->file);
    fwrite(&block_align, 2, 1, b->file);
    fwrite(&bits, 2, 1, b->file);
    fwrite("data", 1, 4, b->file);
    fwrite(&data_bytes, 4, 1, b->file);
    fflush(b->file);
    (void)pos;
    return 0;
}

void wb_backend_stop(wb_backend *b) {
    if (!b) return;
    if (b->kind == 1) {
        if (b->file) { finalize_wav(b); fclose(b->file); }
        b->file = NULL;
        return;
    }
    if (b->device) {
        SDL_PauseAudioDevice(b->device, 1);
        SDL_CloseAudioDevice(b->device);
        b->device = 0;
    }
}

void wb_backend_destroy(wb_backend *b) {
    if (!b) return;
    wb_backend_stop(b);
    free(b);
}
