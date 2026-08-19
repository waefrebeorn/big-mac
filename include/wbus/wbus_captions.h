/* wbus_captions.h — auto-captions API for the video editor.
 *
 * Generate SRT captions from video audio via whisper-cli, burn them
 * into an exported video via ffmpeg subtitles filter.
 *
 * Phase 1 (R011): whisper-cli subprocess. No libwhisper linking needed.
 */

#ifndef WUBUS_WBUS_CAPTIONS_H
#define WUBUS_WBUS_CAPTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

typedef struct wb_captions wb_captions;
typedef struct wb_transcript wb_transcript;  /* G6 bridge (see wbus_transcript.h) */

/* Create/free a captions context. */
wb_captions *wb_captions_create(void);
void         wb_captions_free(wb_captions *c);

/* Generate captions from a video file.
 * Steps: extract audio → whisper-cli transcribe → read SRT + TXT.
 * Returns 0 on success, -1 on error. */
int wb_captions_generate(wb_captions *c, const char *video_path);

/* Get the last transcript text (NULL if not generated yet). */
const char *wb_captions_get_transcript(wb_captions *c);

/* Get the last SRT content (NULL if not generated yet). */
const char *wb_captions_get_srt(wb_captions *c);

/* Write SRT from transcript text. Splits into ~5s segments.
 * `duration_ms` is total clip duration. Returns 0 on success. */
int wb_captions_write_srt(const char *srt_path, const char *text, int duration_ms);

/* Burn SRT captions into a video file via ffmpeg subtitles filter.
 * Returns 0 on success, -1 on error. */
int wb_captions_burn(const char *input_path, const char *srt_path,
                     const char *output_path);

/* G10: ASS (SubStation Alpha) styled-caption support. Parses a minimal but
 * real subset of the ASS Dialogue line (inline overrides \b \i \c&HBBGGRR&
 * \pos(x,y) \move(x1,y1,x2,y2)) so the editor can read/display styled
 * captions, and burns an .ass file directly via ffmpeg's subtitles filter
 * (which renders ASS styling natively). */
typedef struct wb_ass_line {
    int   start_ms;
    int   end_ms;
    char  text[2048];
    int   bold;       /* from \b1 / style */
    int   italic;     /* from \i1 / style */
    int   color_rgb;  /* 0xRRGGBB, -1 if unset */
    int   pos_x;      /* -1 if no \pos */
    int   pos_y;      /* -1 if no \pos */
} wb_ass_line;

/* Parse an ASS file's Dialogue lines into `out` (capacity `max`).
 * Returns the number of lines parsed, or -1 on error. */
int wb_ass_extract_dialogue(const char *ass_path, wb_ass_line *out, int max);

/* Burn an .ass file (with full styling) into a video via ffmpeg. */
int wb_captions_burn_ass(const char *input_path, const char *ass_path,
                          const char *output_path);

/* Clean up temporary files from a captions generation. */
void wb_captions_cleanup(wb_captions *c);

/* G6: get the parsed word-level transcript model (NULL if not generated).
 * Lazily parses the SRT on first call. */
wb_transcript *wb_captions_get_transcript_model(wb_captions *c);

/* Shared helper: run a shell command, return exit code.
 * Declared here so wb_video.c can call it for proxy generation. */
int run_cmd(const char *cmd, const char *context);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_CAPTIONS_H */
