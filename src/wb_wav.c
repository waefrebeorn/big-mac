/*
 * wb_wav.c — hand-written 16-bit PCM WAV writer (strict C11, no libc FILE
 * tricks beyond fwrite — this is the whole point: no third party).
 */
#include "wb_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wr32(unsigned char *b, unsigned long v) {
    b[0] = (unsigned char)(v & 0xFF);
    b[1] = (unsigned char)((v >> 8) & 0xFF);
    b[2] = (unsigned char)((v >> 16) & 0xFF);
    b[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void wr16(unsigned char *b, unsigned v) {
    b[0] = (unsigned char)(v & 0xFF);
    b[1] = (unsigned char)((v >> 8) & 0xFF);
}

int wb_wav_write(const char *path, const double *samples, size_t n, int sample_rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    unsigned long data_bytes = (unsigned long)n * 2;
    unsigned long riff_size = 36 + data_bytes;

    unsigned char hdr[44];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, "RIFF", 4);
    wr32(hdr + 4, riff_size);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    wr32(hdr + 16, 16);                 /* fmt chunk size */
    wr16(hdr + 20, 1);                  /* PCM */
    wr16(hdr + 22, 1);                  /* mono */
    wr32(hdr + 24, (unsigned long)sample_rate);
    wr32(hdr + 28, (unsigned long)sample_rate * 2); /* byte rate */
    wr16(hdr + 32, 2);                  /* block align */
    wr16(hdr + 34, 16);                 /* bits per sample */
    memcpy(hdr + 36, "data", 4);
    wr32(hdr + 40, data_bytes);

    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return -1; }

    /* convert + write in 4096-sample chunks */
    unsigned char buf[4096 * 2];
    size_t i = 0;
    while (i < n) {
        size_t chunk = n - i; if (chunk > 4096) chunk = 4096;
        for (size_t k = 0; k < chunk; k++) {
            double s = samples[i + k];
            if (s < -1) s = -1; if (s > 1) s = 1;
            short v = (short)(s * 32767);
            buf[k * 2] = (unsigned char)(v & 0xFF);
            buf[k * 2 + 1] = (unsigned char)((v >> 8) & 0xFF);
        }
        if (fwrite(buf, 1, chunk * 2, f) != chunk * 2) { fclose(f); return -1; }
        i += chunk;
    }
    fclose(f);
    return 0;
}
