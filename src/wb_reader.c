/*
 * wb_reader.c — 16-bit PCM WAV reader (strict C11, no third party)
 */
#include "wb_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wb_audio_free(wb_audio_t *a) {
    if (!a) return;
    free(a->data);
    a->data = NULL;
    a->n = 0;
}

static unsigned rd32(const unsigned char *b) {
    return (unsigned)b[0] | ((unsigned)b[1] << 8) |
           ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
}
static unsigned rd16(const unsigned char *b) {
    return (unsigned)b[0] | ((unsigned)b[1] << 8);
}

int wb_audio_read(const char *path, wb_audio_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    memset(out, 0, sizeof(*out));

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12) { fclose(f); return -1; }
    if (memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return -1; }

    int sample_rate = 0, channels = 0, bits = 0;
    size_t data_bytes = 0;

    for (;;) {
        unsigned char chunk[8];
        if (fread(chunk, 1, 8, f) != 8) break;
        size_t sz = rd32(chunk + 4);
        if (!memcmp(chunk, "fmt ", 4)) {
            unsigned char fmt[16];
            size_t rd = sz < 16 ? sz : 16;
            if (fread(fmt, 1, rd, f) != rd) { fclose(f); return -1; }
            channels = rd16(fmt + 2);
            sample_rate = (int)rd32(fmt + 4);
            bits = rd16(fmt + 14);
            if (sz > rd) { if (fseek(f, (long)(sz - rd), SEEK_CUR)) { fclose(f); return -1; } }
        } else if (!memcmp(chunk, "data", 4)) {
            data_bytes = sz;
            break;
        } else {
            if (fseek(f, (long)sz, SEEK_CUR)) { fclose(f); return -1; }
        }
    }
    if (sample_rate <= 0 || channels <= 0 || bits != 16) { fclose(f); return -1; }

    size_t frames = data_bytes / (size_t)channels / 2;
    out->sample_rate = sample_rate;
    out->channels = channels;
    out->n = frames;
    out->data = malloc(frames * sizeof(double));
    if (!out->data) { fclose(f); return -1; }

    unsigned char *buf = malloc(data_bytes ? data_bytes : 1);
    if (!buf) { free(out->data); out->data = NULL; fclose(f); return -1; }
    size_t got = fread(buf, 1, data_bytes, f);
    size_t usable = got / (size_t)channels / 2;
    for (size_t i = 0; i < usable; i++) {
        long acc = 0;
        for (int c = 0; c < channels; c++) {
            size_t off = (i * (size_t)channels + (size_t)c) * 2;
            short v = (short)((unsigned short)buf[off] | ((unsigned short)buf[off + 1] << 8));
            acc += v;
        }
        out->data[i] = (double)acc / (double)channels / 32768.0;
    }
    out->n = usable;
    free(buf);
    fclose(f);
    return 0;
}
