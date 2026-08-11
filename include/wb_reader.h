/*
 * wb_reader.h — hand-written 16-bit PCM WAV reader (no third party)
 */
#ifndef WB_READER_H
#define WB_READER_H

#include <stddef.h>

typedef struct {
    int sample_rate;
    int channels;
    size_t n;         /* samples per channel */
    double *data;     /* mono-mixed samples in [-1,1] */
} wb_audio_t;

/* Read a 16-bit PCM WAV. Returns 0 on success, -1 on error.
 * Caller frees with wb_audio_free(). */
int wb_audio_read(const char *path, wb_audio_t *out);
void wb_audio_free(wb_audio_t *a);

#endif /* WB_READER_H */
