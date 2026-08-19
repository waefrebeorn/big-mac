/* wbus_video.h — video track + FFmpeg decode + proxy + preview for the video editor.
 *
 * Architecture (R009):
 *   - Video tracks (kind=video) carry wb_video_clip structs
 *   - wb_video_decoder: FFmpeg C API decode (libavformat/libavcodec/libswscale)
 *   - 480p proxy generated at import via ffmpeg CLI
 *   - SDL2 preview: decode frame → RGBA → SDL2 texture → blit
 *   - Export: ffmpeg CLI with full-res sources + subtitle overlay
 */

#ifndef WUBUS_WBUS_VIDEO_H
#define WUBUS_WBUS_VIDEO_H

#include <stdint.h>
#include <stddef.h>
#include <SDL.h>

/* ---- canonical FFmpeg binary -----------------------------------------
 * The full-featured tessus build (has libavfilter: blackdetect,
 * silencedetect, select/scene, concat demuxer, subtitles). The homebrew
 * static build under ~/homebrew/bin/ffmpeg is a MINIMAL copy/encode-only
 * build with NO filters, so every filter-based feature (lossless trim,
 * segment detect, caption burn) must use this one. */
#ifndef FFmpeg_BIN
#define FFmpeg_BIN "/Users/waefrebeorn/.local/bin/ffmpeg"
#endif

/* ---- video clip (stored on a video track) ----------------------------- */

typedef struct wb_video_clip {
    char source_path[512];   /* original full-res file */
    char proxy_path[512];    /* 480p proxy (mp4/h264) */
    double start_in_source;  /* seconds into the source to start playing */
    double duration;         /* seconds to play from source */
    double timeline_pos;     /* seconds on the timeline where this clip starts */
} wb_video_clip;

void wb_video_clip_init(wb_video_clip *c);
void wb_video_clip_free(wb_video_clip *c);

/* ---- FFmpeg video decoder --------------------------------------------- */

typedef struct wb_video_decoder wb_video_decoder;

wb_video_decoder *wb_video_decoder_open(const char *path);
void               wb_video_decoder_close(wb_video_decoder *d);

/* Seek to a time in seconds. Returns 0 on success. */
int wb_video_decoder_seek(wb_video_decoder *d, double time_sec);

/* Decode one frame at current position into RGBA buffer (caller provides).
 * out_w/out_h: output dimensions (set by caller, usually PROXY_SCALE_W/H).
 * Returns 0 on success, -1 on EOF/error. */
int wb_video_decoder_decode_frame(wb_video_decoder *d, uint8_t *out,
                                   int *out_w, int *out_h);

double wb_video_decoder_get_duration(wb_video_decoder *d);
int    wb_video_decoder_get_width(wb_video_decoder *d);
int    wb_video_decoder_get_height(wb_video_decoder *d);

/* ---- proxy generation (ffmpeg CLI) ------------------------------------ */

int wb_video_make_proxy(const char *src, const char *proxy);
double wb_video_proxy_duration(const char *proxy_path);

/* ---- SDL2 preview helpers ---------------------------------------------- */

SDL_Texture *wb_video_frame_to_texture(SDL_Renderer *ren, uint8_t *rgba_data,
                                        int w, int h);
void         wb_video_blit_scaled(SDL_Renderer *ren, SDL_Texture *tex,
                                   SDL_Rect *dst);

/* ---- lossless trim + auto-cut (R016 S4: LosslessCut concat demuxer) --- */

/* A keep-segment: [start, end] seconds to retain from the source.
 * The exported file contains exactly these segments concatenated, in order,
 * with NO re-encode (stream copy). This is the lossless trim/cut path. */
typedef struct wb_video_segment {
    double start;   /* seconds, inclusive */
    double end;     /* seconds, exclusive; end <= 0 means "to end of source" */
} wb_video_segment;

/* Lossless multi-segment trim: remux the given keep-segments via the ffmpeg
 * concat demuxer with -c copy. Returns 0 on success, -1 on error.
 * Segments may overlap/gaps; they are concatenated in the order given.
 * A segment whose end <= start is skipped. */
int wb_video_lossless_trim(const char *src, const wb_video_segment *segs,
                           int nsegs, const char *out_path);

/* Detect segments in `src` using ffmpeg filters (no ML, pure ffmpeg):
 *   mode 0 = scene changes (select=gt(scene,thr)), 1 = black, 2 = silence.
 * Fills `out` (caller-allocated, capacity `cap`) with detected [start,end]
 * spans. Returns number of segments written, or -1 on error.
 * `threshold`: scene change sensitivity (0..1, e.g. 0.3) or black/silence
 * duration threshold in seconds (e.g. 1.0). */
int wb_video_detect_segments(const char *src, int mode, double threshold,
                              wb_video_segment *out, int cap);

#endif /* WUBUS_WBUS_VIDEO_H */
