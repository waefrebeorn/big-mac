/* wbus_gif.h — Animated GIF export (R084 video edit graph).
 *
 * Provides wb_export_gif(): renders an edit graph to an animated GIF
 * (GIF89a with LZW compression, global color table, Netscape looping,
 * and per-frame Graphic Control Extensions). Pure C11, no external
 * libraries.
 */

#ifndef WBUS_GIF_H
#define WBUS_GIF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Forward-declare the edit graph (defined in wbus_edit.h, included by
 * .c files that already pull in the edit pipeline). */
typedef struct wb_edit_graph wb_edit_graph;

/* Opaque callback type for progress reporting (same as wb_export_prog_fn). */
typedef void (*wb_export_prog_fn)(void *ctx, double progress);

/* Export the edit graph at `g` to an animated GIF at `path`.
 *
 *   g          : edit graph to render (must have duration > 0)
 *   path       : output file path
 *   width      : output width in pixels (0 = use g->width)
 *   height     : output height in pixels (0 = use g->height)
 *   fps        : frame rate (e.g. 15.0). Clamped to [1, 60].
 *   max_colors : palette size cap, 2..256. Values > 256 are clamped.
 *
 * Frames are evaluated at t = frame/fps for frame in [0, total_frames).
 * Each frame is converted from RGBA float to an indexed palette via
 * median-cut quantization, then written as a GIF89a frame with LZW
 * compression. The GIF loops forever (Netscape extension).
 *
 * honours a cancel flag (NULL = no cancellation).
 * prog callback (may be NULL) invoked with progress 0..1.
 *
 * Returns: 0 on success, -1 on error, -2 if cancelled.
 */
int wb_export_gif(wb_edit_graph *g, const char *path,
                  int width, int height, float fps, int max_colors);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_GIF_H */
