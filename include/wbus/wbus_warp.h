/* wbus_warp.h — R078: Ableton-style warp markers for elastic audio.
 *
 * Warp markers remap a source audio timeline to a musical (beat) timeline.
 * Between adjacent markers, linear time-stretching is applied so audio
 * conforms to the beat grid without pitch shift.
 */

#ifndef WBUS_WBUS_WARP_H
#define WBUS_WBUS_WARP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque warp-marker state. */
typedef struct wb_warp wb_warp;

/* Create a warp-marker instance at the given sample rate. */
wb_warp *wb_warp_create(uint32_t sr);

/* Destroy a warp-marker instance. */
void wb_warp_destroy(wb_warp *w);

/* Set the source audio buffer (interleaved, frames*channels samples).
 * Returns 0 on success, -1 on error. */
int wb_warp_set_source(wb_warp *w, const wb_sample *audio,
                       uint32_t frames, uint32_t channels);

/* Add a warp marker mapping source sample position to beat position.
 * Markers are kept sorted by src_sample. Returns new count or -1. */
int wb_warp_add_marker(wb_warp *w, double src_sample, double dst_beat);

/* Remove a marker by index. Returns 0 on success, -1 on error. */
int wb_warp_remove_marker(wb_warp *w, int index);

/* Remove all markers. Returns 0 on success. */
int wb_warp_clear_markers(wb_warp *w);

/* Return the number of markers currently set. */
int wb_warp_marker_count(const wb_warp *w);

/* Map a source sample position to warped beat position.
 * With no markers, returns the source position unchanged (identity). */
double wb_warp_src_to_dst(const wb_warp *w, double src_sample);

/* Map a beat position back to source sample position.
 * Inverse of wb_warp_src_to_dst. */
double wb_warp_dst_to_src(const wb_warp *w, double beat);

/* Auto-place markers from detected beat positions.
 * beat_positions[] are source-sample positions of detected beats.
 * Maps beat index i → beat i (uniform spacing). Returns count or -1. */
int wb_warp_auto_warp(wb_warp *w, const double *beat_positions, int num_beats);

/* Render warped audio for the beat range [beat_start, beat_end] into
 * interleaved stereo output (frames*2 samples). */
void wb_warp_process(wb_warp *w, double beat_start, double beat_end,
                     wb_sample *out, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_WBUS_WARP_H */