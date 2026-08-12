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
