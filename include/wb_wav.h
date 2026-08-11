/*
 * wb_wav.h — hand-written 16-bit PCM WAV writer (no third party)
 */
#ifndef WB_WAV_H
#define WB_WAV_H

#include <stddef.h>

/* Write mono 16-bit WAV. samples in [-1,1]. Returns 0 on success. */
int wb_wav_write(const char *path, const double *samples, size_t n, int sample_rate);

#endif /* WB_WAV_H */
