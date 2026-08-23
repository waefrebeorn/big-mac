/* wbus_shadowbin.h — the "magic shadow bin" (R061).
 *
 * A sidecar file per project that captures the FULL edit state in a
 * portable JSON shape inspired by OTIO (Timeline > Stack > Track > Clip),
 * so a bigger machine / bigger NLE can:
 *   - read what we did (conform on their side), and
 *   - write back changes we can re-import.
 *
 * This is NOT a transcode: no media is copied. It's the *decision list*
 * — every trim, fade, lane, marker, gain, CGI reference — saved beside
 * the media. Tiny machine edits; big software renders.
 *
 * Atomic writes: temp file + rename, never a truncated project.
 *
 * C11, opaque, self-contained.
 */
#ifndef WUBUS_WBUS_SHADOWBIN_H
#define WUBUS_WBUS_SHADOWBIN_H

#include <stdint.h>
#include <stddef.h>
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Write the session's full edit state as JSON to <path>. The layout:
 * { "otio_schema": "ShadowBin.1",
 *   "name": ..., "bpm": ..., "length_seconds": ...,
 *   "tracks": [ { "kind": "audio|video|instrument", "name": ...,
 *                 "volume": .., "pan": .., "mute": .., "solo": ..,
 *                 "active_lane": ..,
 *                 "clips": [ { "start_s":.., "length_s":..,
 *                              "source": "...", "in_s":.., "lane":..,
 *                              "gain":.. } ] } ],
 *   "markers": [ { "pos_s":.., "label":"...", "kind":0|1 } ],
 *   "cgi": { "hint": "see agent cgi-* commands" } }
 * Returns 0 on success. */
int wb_shadowbin_write(const wb_session *s, const char *path);

/* Read a shadow bin back into the session. Replaces video-track clip
 * layouts and markers whose sources resolve; audio clips restore
 * start/length/lane/gain for clips whose source matches by filename.
 * Returns number of clips restored, or -1 on parse error. */
int wb_shadowbin_read(wb_session *s, const char *path);

/* Convenience: default sidecar path next to a project file
 * ("foo.wbus" -> "foo.shadowbin.json"). Writes buf. */
void wb_shadowbin_path_for(const char *project_path, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_SHADOWBIN_H */
