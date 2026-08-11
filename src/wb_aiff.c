/*
 * wb_aiff.c — AIFF writer: FORM/COMM/SSND chunks, 16-bit big-endian PCM.
 * GarageBand imports AIFF natively (its loop browser prefers it).
 */
#include "wb_aiff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_be32(unsigned char *b, unsigned long v) {
    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)(v);
}

static void put_be16(unsigned char *b, unsigned v) {
    b[0] = (unsigned char)(v >> 8);
    b[1] = (unsigned char)(v);
}

int wb_aiff_write(const char *path, const double *samples, size_t n, int sample_rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    /* FORM chunk */
    fwrite("FORM", 1, 4, f);
    unsigned char sizebuf[4];
    /* FORM content = "AIFF" (4) + COMM chunk (8 hdr + 18) + SSND chunk
     * (8 hdr + 8 offset/blk + 2*n data) = 46 + 2*n */
    put_be32(sizebuf, 4 + 26 + 16 + 2 * n);
    fwrite(sizebuf, 1, 4, f);
    fwrite("AIFF", 1, 4, f);

    /* COMM chunk */
    fwrite("COMM", 1, 4, f);
    put_be32(sizebuf, 18);
    fwrite(sizebuf, 1, 4, f);
    unsigned char comm[18];
    memset(comm, 0, sizeof(comm));
    put_be16(comm, 1);        /* channels */
    put_be32(comm + 2, n);    /* frames */
    put_be16(comm + 6, 16);   /* bits */
    /* 80-bit extended float sample rate (IEEE 754 extended precision).
     * NOTE: unlike double, extended floats store the leading '1.' bit
     * EXPLICITLY in the 64-bit mantissa field. */
    {
        /* normalize: find exponent so mantissa in [1,2) */
        int expo = 0;
        double mant = (double)sample_rate;
        while (mant >= 2.0) { mant /= 2.0; expo++; }
        while (mant < 1.0) { mant *= 2.0; expo--; }
        unsigned long long bits = 1ULL << 63;  /* explicit leading bit */
        double frac = mant - 1.0;              /* fraction after the '1.' */
        for (int b = 0; b < 63; b++) {
            frac *= 2.0;
            if (frac >= 1.0) { bits |= (1ULL << (62 - b)); frac -= 1.0; }
        }
        int ebias = 16383 + expo;
        comm[8]  = (unsigned char)(ebias >> 8);
        comm[9]  = (unsigned char)(ebias & 0xFF);
        comm[10] = (unsigned char)(bits >> 56);
        comm[11] = (unsigned char)(bits >> 48);
        comm[12] = (unsigned char)(bits >> 40);
        comm[13] = (unsigned char)(bits >> 32);
        comm[14] = (unsigned char)(bits >> 24);
        comm[15] = (unsigned char)(bits >> 16);
        comm[16] = (unsigned char)(bits >> 8);
        comm[17] = (unsigned char)(bits);
    }
    fwrite(comm, 1, 18, f);

    /* SSND chunk */
    fwrite("SSND", 1, 4, f);
    unsigned long data_len = 2 * n;
    put_be32(sizebuf, 8 + data_len);
    fwrite(sizebuf, 1, 4, f);
    put_be32(sizebuf, 0);     /* offset */
    fwrite(sizebuf, 1, 4, f);
    put_be32(sizebuf, 0);     /* block size */
    fwrite(sizebuf, 1, 4, f);

    /* samples, big-endian */
    unsigned char *sbuf = malloc(2 * 8192);
    if (!sbuf) { fclose(f); return -1; }
    size_t i = 0;
    while (i < n) {
        size_t chunk = (n - i) < 8192 ? (n - i) : 8192;
        for (size_t k = 0; k < chunk; k++) {
            double s = samples[i + k];
            if (s > 1.0) s = 1.0;
            if (s < -1.0) s = -1.0;
            short v = (short)(s * 32767.0);
            put_be16(sbuf + 2 * k, (unsigned)v);
        }
        fwrite(sbuf, 1, 2 * chunk, f);
        i += chunk;
    }
    free(sbuf);
    fclose(f);
    return 0;
}
