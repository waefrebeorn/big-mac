/* wbus_perfclip.h — performance recordings as nested timeline clips (R068).
 *
 * The closing integration: a recorded performance becomes a first-class
 * timeline element. A PERF clip stores its event list (the reproducible
 * element — no media), and the compositor can rasterize it over any time
 * window via the deterministic replay in wb_perf.
 *
 * Serialization is JSON (same policy as shadow bins) so the sidecar
 * round-trips through projects and big NLEs alike. */
#ifndef WUBUS_WBUS_PERFCLIP_H
#define WUBUS_WBUS_PERFCLIP_H

#include <stdint.h>
#include <stddef.h>
#include "wbus.h"
#include "wbus_perf.h"
#include "wbus_perf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An opaque handle to a perf clip: owns its event snapshot + deck list
 * (mesh refs are NOT owned — decks live in the session like any clip). */
typedef struct wb_perfclip wb_perfclip;

/* Snapshot a live perf into a clip at time `start` (seconds). The clip
 * becomes self-contained: replay no longer needs the live perf handle. */
wb_perfclip *wb_perfclip_snapshot(wb_session *s, wb_perf *perf,
                                  double start, double duration);

void wb_perfclip_free(wb_perfclip *pc);

/* Rasterize the perf onto `rgba` (W*H*4) at perf-time t (seconds). */
int wb_perfclip_render(wb_perfclip *pc, double t,
                       uint8_t *rgba, int w, int h);

/* Number of events carried by this clip (snapshot of the live perf log). */
int  wb_perfclip_event_count(const wb_perfclip *pc);

/* JSON round-trip (matches shadow-bin schema: atomic tmp+rename). */
int wb_perfclip_save(const wb_perfclip *pc, const char *path);

/* R070: rasterize the perf clip over [t0, t0+dur) at `fps` into a raw
 * RGBA file (one frame after another, W*H*4 each) — the same interchange
 * format wb_video_export_cgi's overlay path consumes. Returns 0 or -1. */
int wb_perfclip_render_seq(const wb_perfclip *pc,
                           double t0, double dur, double fps,
                           int w, int h, const char *raw_path);

#ifdef __cplusplus
}
#endif
#endif /* WUBUS_WBUS_PERFCLIP_H */
