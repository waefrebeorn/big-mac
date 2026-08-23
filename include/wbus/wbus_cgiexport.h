/* wbus_cgiexport.h — render wb_anim frames and composite them over video
 * via the ffmpeg overlay filter (R058). This is the last mile of the CGI
 * pipeline: AGI authors a wb_anim, this turns it into pixels on top of
 * real footage in the final export.
 *
 * Two paths:
 *   1. wb_cgi_render_seq(): animation -> PNG sequence (for inspection)
 *   2. wb_video_export_cgi(): full export = session video + audio +
 *      captions (optional) + ANIMATION OVERLAY (optional), one ffmpeg pass.
 *
 * C11; ffmpeg CLI only (same binary policy as the rest of wb_video).
 */
#ifndef WUBUS_WBUS_CGIEXPORT_H
#define WUBUS_WBUS_CGIEXPORT_H

#include <stdint.h>
#include <stddef.h>
#include "wbus_anim.h"
#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Render anim frames [t0, t0+dur) at fps into out_dir/frame_%04d.png.
 * Frames are RGBA composited onto opaque black where nothing draws —
 * for the SEQUENCE path only; the export path uses transparent rawvideo.
 * Returns frame count or -1. */
int wb_cgi_render_seq(wb_anim *a, double t0, double dur, double fps,
                      const char *out_dir);

/* Overlay options for export. alpha=1 keeps transparency keyed correctly;
 * scale maps the anim's internal resolution to output resolution. */
typedef struct {
    wb_anim *anim;        /* NULL = no overlay */
    double   t_start;     /* timeline seconds when overlay begins */
    double   duration;    /* seconds it lasts */
} wb_cgi_overlay;

/* Full export with optional caption + CGI overlay. Same contract as
 * wb_video_export_codec plus the overlay. Returns 0 on success. */
int wb_video_export_cgi(wb_session *s, wb_engine *e,
                        const char *output_path,
                        const char *srt_path,
                        wb_video_codec codec,
                        const wb_cgi_overlay *overlay);

/* R064: same as above but normalizes the rendered audio to target LUFS
 * (two-pass EBU R128) BEFORE the video mux — a one-call platform-ready
 * master. target_lufs 0 disables (plain export). */
int wb_video_export_delivery(wb_session *s, wb_engine *e,
                             const char *output_path,
                             const char *srt_path,
                             wb_video_codec codec,
                             const wb_cgi_overlay *overlay,
                             double target_lufs);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_CGIEXPORT_H */
