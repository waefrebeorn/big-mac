/*
 * bigmac_out.c -- the host audio output backend for WuBuOS.
 * See bigmac_out.h for the contract. Pulse -> ALSA -> WAV, best effort.
 */
#include "bigmac_out.h"

/* ALSA's <alsa/global.h> redefines struct timeval/timespec unless a POSIX
 * feature macro is in effect; pulse's headers pull in <time.h> first and the
 * two collide. One line here, before any system header, settles it. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- PulseAudio ------------------------------------------------------- */
#include <pulse/simple.h>
#include <pulse/error.h>

/* -- ALSA ------------------------------------------------------------- */
#include <alsa/asoundlib.h>
#include <alloca.h>

static BigmacBackend g_backend = BIGMAC_BACKEND_NONE;
static int  g_rate;
static int  g_chan;

/* pulse state */
static pa_simple *g_pa;

/* alsa state */
static snd_pcm_t *g_pcm;

/* wav state */
static FILE    *g_wav;
static long     g_wav_frames;
static char     g_wav_path[512];

/* frames accepted by the current backend but not yet heard */
static int g_latency;

static int open_pulse(void)
{
    pa_sample_spec ss;
    ss.format   = PA_SAMPLE_FLOAT32LE;
    ss.rate     = (uint32_t)g_rate;
    ss.channels = (uint8_t)g_chan;

    pa_buffer_attr attr;
    attr.maxlength = (uint32_t)-1;
    attr.tlength   = (uint32_t)(g_rate / 20 * g_chan * 4);  /* 50 ms */
    attr.prebuf    = (uint32_t)-1;
    attr.minreq    = (uint32_t)-1;
    attr.fragsize = (uint32_t)-1;

    int err = 0;
    g_pa = pa_simple_new(NULL, "WuBuOS", PA_STREAM_PLAYBACK, NULL,
                         "wubu audio", &ss, NULL, &attr, &err);
    if (!g_pa)
        return -1;

    g_backend = BIGMAC_BACKEND_PULSE;
    return 0;
}

static int open_alsa(void)
{
    if (snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
        return -1;

    snd_pcm_hw_params_t *p = NULL;
    snd_pcm_hw_params_alloca(&p);
    if (snd_pcm_hw_params_any(g_pcm, p) < 0) {
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }
    if (snd_pcm_hw_params_set_access(g_pcm, p,
            SND_PCM_ACCESS_RW_INTERLEAVED) < 0 ||
        snd_pcm_hw_params_set_format(g_pcm, p, SND_PCM_FORMAT_FLOAT_LE) < 0 ||
        snd_pcm_hw_params_set_channels(g_pcm, p, (unsigned)g_chan) < 0 ||
        snd_pcm_hw_params_set_rate(g_pcm, p, (unsigned)g_rate, 0) < 0 ||
        snd_pcm_hw_params(g_pcm, p) < 0) {
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }
    snd_pcm_prepare(g_pcm);
    g_backend = BIGMAC_BACKEND_ALSA;
    return 0;
}

static int open_wav(const char *path)
{
    if (!path || !*path)
        path = "/tmp/wubu_audio.wav";
    g_wav = fopen(path, "wb");
    if (!g_wav)
        return -1;

    snprintf(g_wav_path, sizeof(g_wav_path), "%s", path);
    g_wav_frames = 0;

    /* a 44-byte canonical header, patched on close */
    uint8_t hdr[44];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr,     "RIFF", 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t one = 1, fmtlen = 16;
    uint32_t rate = (uint32_t)g_rate;
    uint16_t ch = (uint16_t)g_chan;
    uint16_t bits = 32;                     /* float32 */
    memcpy(hdr + 16, &fmtlen, 4);
    memcpy(hdr + 20, &one, 2);              /* IEEE float */
    memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &rate, 4);
    uint32_t byterate = rate * ch * 4;
    uint16_t align = (uint16_t)(ch * 4);
    memcpy(hdr + 28, &byterate, 4);
    memcpy(hdr + 32, &align, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    fwrite(hdr, 1, 44, g_wav);

    g_backend = BIGMAC_BACKEND_WAV;
    return 0;
}

int bigmac_out_init(int sample_rate, int channels, const char *wav_path)
{
    g_rate = sample_rate > 0 ? sample_rate : 48000;
    g_chan = (channels == 1 || channels == 2) ? channels : 2;
    g_latency = 0;

    /* BIGMAC_BACKEND=pulse|alsa|wav forces one backend (a headless install,
     * or a test that needs the wav path, uses this). */
    const char *force = getenv("BIGMAC_BACKEND");
    if (force && *force) {
        if (strcmp(force, "pulse") == 0) {
            if (open_pulse() == 0) return 0;
            return -1;
        }
        if (strcmp(force, "alsa") == 0) {
            if (open_alsa() == 0) return 0;
            return -1;
        }
        if (strcmp(force, "wav") == 0)
            return open_wav(wav_path);
    }

    if (open_pulse() == 0)
        return 0;
    if (open_alsa() == 0)
        return 0;
    return open_wav(wav_path);
}

long bigmac_out_write(const float *frames, int nframes)
{
    if (!frames || nframes <= 0)
        return 0;

    switch (g_backend) {
    case BIGMAC_BACKEND_PULSE: {
        int err = 0;
        if (pa_simple_write(g_pa, frames,
                            (size_t)nframes * g_chan * sizeof(float), &err) < 0)
            return -1;
        g_latency += nframes;
        return nframes;
    }
    case BIGMAC_BACKEND_ALSA: {
        snd_pcm_sframes_t n = snd_pcm_writei(g_pcm, frames, (snd_pcm_uframes_t)nframes);
        if (n < 0) {
            /* underrun is recoverable; resynchronize and report 0 */
            if (snd_pcm_recover(g_pcm, (int)n, 1) < 0)
                return -1;
            return 0;
        }
        g_latency += (int)n;
        return (long)n;
    }
    case BIGMAC_BACKEND_WAV:
        if (!g_wav)
            return -1;
        fwrite(frames, sizeof(float),
               (size_t)nframes * g_chan, g_wav);
        g_wav_frames += nframes;
        return nframes;
    default:
        return -1;
    }
}

int bigmac_out_latency_frames(void)
{
    if (g_backend == BIGMAC_BACKEND_PULSE && g_pa) {
        int err = 0;
        pa_usec_t t = pa_simple_get_latency(g_pa, &err);
        if (err == PA_OK && t > 0)
            return (int)(t * (pa_usec_t)g_rate / 1000000);
    }
    if (g_backend == BIGMAC_BACKEND_ALSA && g_pcm) {
        snd_pcm_sframes_t d = 0;
        if (snd_pcm_delay(g_pcm, &d) == 0 && d > 0)
            return (int)d;
    }
    return g_latency;
}

BigmacBackend bigmac_out_backend(void) { return g_backend; }

const char *bigmac_out_backend_name(void)
{
    switch (g_backend) {
    case BIGMAC_BACKEND_PULSE: return "pulse";
    case BIGMAC_BACKEND_ALSA:  return "alsa";
    case BIGMAC_BACKEND_WAV:   return "wav";
    default:                   return "none";
    }
}

void bigmac_out_close(void)
{
    if (g_backend == BIGMAC_BACKEND_PULSE && g_pa) {
        int err = 0;
        pa_simple_drain(g_pa, &err);
        pa_simple_free(g_pa);
        g_pa = NULL;
    }
    if (g_backend == BIGMAC_BACKEND_ALSA && g_pcm) {
        snd_pcm_drain(g_pcm);
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
    }
    if (g_backend == BIGMAC_BACKEND_WAV && g_wav) {
        /* patch the RIFF/data sizes now that we know them */
        long end = ftell(g_wav);
        fseek(g_wav, 4, SEEK_SET);
        uint32_t riff = (uint32_t)(end - 8);
        fwrite(&riff, 4, 1, g_wav);
        fseek(g_wav, 40, SEEK_SET);
        uint32_t data = (uint32_t)(end - 44);
        fwrite(&data, 4, 1, g_wav);
        fclose(g_wav);
        g_wav = NULL;
    }
    g_backend = BIGMAC_BACKEND_NONE;
}
