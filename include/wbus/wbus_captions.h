/* wbus_captions.h — auto-captions API for the video editor.
 *
 * Generate SRT captions from video audio via whisper-cli, burn them
 * into an exported video via ffmpeg subtitles filter.
 *
 * Phase 1 (R011): whisper-cli subprocess. No libwhisper linking needed.
 */

#ifndef WUBUS_WBUS_CAPTIONS_H
#define WUBUS_WBUS_CAPTIONS_H

#include <stdint.h>
#include <stddef.h>

typedef struct wb_captions wb_captions;

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

/* Clean up temporary files from a captions generation. */
void wb_captions_cleanup(wb_captions *c);

/* Shared helper: run a shell command, return exit code.
 * Declared here so wb_video.c can call it for proxy generation. */
int run_cmd(const char *cmd, const char *context);

#endif /* WUBUS_WBUS_CAPTIONS_H */
