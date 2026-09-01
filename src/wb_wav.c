/* wb_wav.c — WAV file reader/writer (16-bit PCM and 32-bit float).
 * Pure C11, all ours. Standard RIFF/WAVE container.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus.h"

/* Write an interleaved float sample buffer to a 16-bit PCM WAV file.
 * Returns 0 on success, -1 on failure. */
int wb_wav_write_pcm16(const char *path,
                       const wb_sample *data, uint32_t frames,
                       uint8_t channels, uint32_t sample_rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    uint32_t data_bytes = frames * channels * 2;
    uint32_t riff_size = 36 + data_bytes;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    uint16_t audio_format = 1;          /* PCM */
    uint16_t num_channels = channels;
    uint32_t byte_rate = sample_rate * channels * 2;
    uint16_t block_align = (uint16_t)(channels * 2);
    uint16_t bits = 16;

    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);

    /* data chunk */
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);

    /* samples */
    for (uint32_t i = 0; i < frames * channels; i++) {
        float v = data[i];
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        int16_t s = (int16_t)(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }

    fclose(f);
    return 0;
}

/* Write a 32-bit float WAV (for lossless internal renders). */
int wb_wav_write_f32(const char *path,
                     const wb_sample *data, uint32_t frames,
                     uint8_t channels, uint32_t sample_rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    uint32_t data_bytes = frames * channels * 4;
    uint32_t riff_size = 36 + data_bytes;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    uint16_t audio_format = 3;          /* IEEE float */
    uint16_t num_channels = channels;
    uint32_t byte_rate = sample_rate * channels * 4;
    uint16_t block_align = (uint16_t)(channels * 4);
    uint16_t bits = 32;

    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);

    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
    fwrite(data, 4, frames * channels, f);

    fclose(f);
    return 0;
}

/* Read a 16-bit PCM WAV into an interleaved float buffer (caller-owned).
 * On success sets out_frames / out_channels / out_sr and returns 0; the buffer
 * must be freed by the caller. Returns -1 on error. */
int wb_wav_read_pcm16(const char *path, float **out_data, uint32_t *out_frames,
                      int *out_channels, int *out_sr) {
    if (!path || !out_data) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char tag[4];
    if (fread(tag, 1, 4, f) != 4 || memcmp(tag, "RIFF", 4) != 0) { fclose(f); return -1; }
    uint32_t riff = 0; fread(&riff, 4, 1, f); (void)riff;
    if (fread(tag, 1, 4, f) != 4 || memcmp(tag, "WAVE", 4) != 0) { fclose(f); return -1; }

    int channels = 0, sr = 0;
    uint32_t data_bytes = 0;
    int found_fmt = 0, found_data = 0;
    while (!found_data && fread(tag, 1, 4, f) == 4) {
        uint32_t chunk = 0;
        if (fread(&chunk, 4, 1, f) != 1) break;
        if (memcmp(tag, "fmt ", 4) == 0) {
            uint16_t fmt = 0, ch = 0, bits = 0;
            uint32_t rate = 0;
            fread(&fmt, 2, 1, f);
            fread(&ch, 2, 1, f);
            fread(&rate, 4, 1, f);
            fseek(f, 6, SEEK_CUR);          /* byte_rate(4) + block_align(2) */
            fread(&bits, 2, 1, f);
            channels = ch; sr = (int)rate; (void)fmt; (void)bits;
            found_fmt = 1;
            if (chunk > 16) fseek(f, (long)(chunk - 16), SEEK_CUR);
        } else if (memcmp(tag, "data", 4) == 0) {
            data_bytes = chunk;
            found_data = 1;
        } else {
            fseek(f, (long)chunk, SEEK_CUR);
        }
    }
    if (!found_fmt || !found_data || channels <= 0 || data_bytes == 0) {
        fclose(f); return -1;
    }
    uint32_t n_samples = data_bytes / 2;   /* 16-bit samples (interleaved) */
    float *buf = malloc((size_t)n_samples * sizeof(float));
    if (!buf) { fclose(f); return -1; }
    for (uint32_t i = 0; i < n_samples; i++) {
        int16_t s = 0;
        if (fread(&s, 2, 1, f) != 1) { free(buf); fclose(f); return -1; }
        buf[i] = (float)s / 32768.0f;
    }
    fclose(f);
    *out_data = buf;
    *out_frames = channels > 0 ? n_samples / (uint32_t)channels : n_samples;
    *out_channels = channels;
    *out_sr = sr;
    return 0;
}

/* Write a Broadcast Wave Format (BWF) file (Vegas 10 feature).
 * BWF is a WAV extension with metadata chunks (bext, levl).
 * Returns 0 on success, -1 on failure. */
int wb_wav_write_bwf(const char *path, const wb_sample *data, uint32_t frames,
                      uint8_t channels, uint32_t sample_rate,
                      const char *description, const char *originator,
                      const char *originator_ref, time_t orig_time) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    uint32_t data_bytes = frames * channels * 2;

    /* bext chunk (Broadcast Extension) */
    uint32_t bext_size = 602;  /* minimum bext size */
    uint32_t bext_data_size = 602;
    char bext[602];
    memset(bext, 0, sizeof(bext));
    if (description) {
        int len = strlen(description);
        if (len > 256) len = 256;
        memcpy(bext, description, len);
    }
    if (originator) {
        int len = strlen(originator);
        if (len > 32) len = 32;
        memcpy(bext + 256, originator, len);
    }
    if (originator_ref) {
        int len = strlen(originator_ref);
        if (len > 32) len = 32;
        memcpy(bext + 288, originator_ref, len);
    }
    /* origination time (8 bytes, HH:MM:SS) */
    struct tm *tm = gmtime(&orig_time);
    if (tm) {
        snprintf(bext + 320, 9, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
    /* origination date (10 bytes, YYYY:MM:DD) */
    if (tm) {
        snprintf(bext + 328, 11, "%04d:%02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    }
    /* time reference (8 bytes, sample count since midnight) */
    uint64_t time_ref = 0;
    memcpy(bext + 338, &time_ref, 8);
    /* version (2 bytes) */
    uint16_t version = 1;
    memcpy(bext + 346, &version, 2);
    /* UMID (64 bytes) — left zeros */
    /* loudness values (16 bytes) — left zeros */
    /* reserved (180 bytes) — left zeros */
    /* coding history (variable) — left empty */

    uint32_t riff_size = 36 + bext_data_size + 8 + data_bytes;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* bext chunk */
    fwrite("bext", 1, 4, f);
    fwrite(&bext_size, 4, 1, f);
    fwrite(bext, 1, bext_data_size, f);

    /* fmt chunk */
    uint16_t audio_format = 1;
    uint16_t num_channels = channels;
    uint32_t byte_rate = sample_rate * channels * 2;
    uint16_t block_align = (uint16_t)(channels * 2);
    uint16_t bits = 16;

    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);

    /* data chunk */
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);

    /* samples */
    for (uint32_t i = 0; i < frames * channels; i++) {
        float v = data[i];
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        int16_t s = (int16_t)(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }

    fclose(f);
    return 0;
}
