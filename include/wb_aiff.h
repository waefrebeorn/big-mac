/*
 * wb_aiff.h — hand-written AIFF writer (no third party, GarageBand-native)
 */
#ifndef WB_AIFF_H
#define WB_AIFF_H

#include <stddef.h>

/* Write mono 16-bit AIFF. samples in [-1,1]. Returns 0 on success. */
int wb_aiff_write(const char *path, const double *samples, size_t n, int sample_rate);

#endif /* WB_AIFF_H */
