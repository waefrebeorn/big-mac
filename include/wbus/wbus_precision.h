/* wbus_precision.h — Wave 2 lane B: G15/G16/G66 precision-edit helpers.
 *
 * Pure session-math used by the EDIT-view trim mode (G15), the razor tool
 * (G16) and the drop modes (G66). Factored here so the gate can verify the
 * math headless without any UI or media files (clips are built manually in
 * tests, exactly like test_video_edit).
 */
#ifndef WUBUS_PRECISION_H
#define WUBUS_PRECISION_H

#include "wbus.h"

/* G15: nudge an edit point of video clip `clip` on `track` by `delta`
 * seconds (frame-wise: caller passes e.g. +/-1/25.0).
 *   edge 0 = in-point  (clip start): delta>0 moves the cut later —
 *           clip shrinks from the head, start_in_source advances; if the
 *           PREVIOUS clip abuts, its tail rolls longer (roll edit).
 *   edge 1 = out-point (clip end):   delta>0 extends the clip; if the NEXT
 *           clip abuts, its head shrinks and its source window advances.
 * Both sides clamp to a minimum remaining length of one frame (1/25s).
 * Returns 0 on success, -1 on invalid args / non-video clip. */
int wb_session_nudge_edit_point(wb_session *s, int track, int clip,
                                int edge, double delta);

/* G15: pick the edit point (edge) of `clip` nearest to timeline time `t`.
 * Writes 0 (in) or 1 (out) into *edge; returns 0 on success. */
int wb_precision_nearest_edge(wb_session *s, int track, int clip,
                              double t, int *edge);

/* G16: razor — split EVERY video clip under timeline time `t` (seconds).
 * When all_tracks != 0 every track is cut; otherwise only `track`.
 * Returns the number of splits performed (0 = nothing under the blade),
 * or -1 on invalid args. Uses wb_session_split_video_clip per clip so the
 * source windows stay frame-exact. */
int wb_session_razor_split_all_at_time(wb_session *s, double t,
                                       int track, int all_tracks);

/* G66: drop modes. WB_DROP_OVERWRITE places freely (default);
 * WB_DROP_INSERT first ripples every later clip on the track right by the
 * placed clip's span so nothing is covered up. pos/len use the SAME unit as
 * the track's clips (seconds for video tracks, samples for audio/MIDI).
 * Returns the number of clips shifted (0 in overwrite mode), -1 on error. */
typedef enum { WB_DROP_OVERWRITE = 0, WB_DROP_INSERT = 1 } wb_drop_mode;
int wb_session_drop_place(wb_session *s, int track, double pos, double len,
                          wb_drop_mode mode);

#endif /* WUBUS_PRECISION_H */
