#ifndef WBUS_IMPORT_H
#define WBUS_IMPORT_H

/* Wave 1 G01/G02 — media import: directory scan + audio-file import.
 *
 * G01: wb_import_scan_dir enumerates media files (*.mp4 *.mov *.wav
 *      *.aiff *.mp3 *.m4a) in a directory, sorted by name, so the GUI
 *      browser can list them without any external library.
 * G02: wb_import_audio_file reads a wav directly (wb_wav_read_pcm16) or
 *      transcodes any other audio-bearing file through the full ffmpeg
 *      binary into a temp pcm_s16le wav, then places it on an (existing or
 *      newly created) AUDIO track at `pos_sec` — same policy as the agent's
 *      import-audio command.
 */

#include "wbus.h"

#define WB_IMPORT_PATH_MAX 1024

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if path has a known media extension (.mp4 .mov .wav .aiff .mp3 .m4a). */
int  wb_import_is_media_path(const char *path);

/* Enumerate media files in `dir` into `out` (sorted by name).
 * Returns the number written, or -1 if the dir cannot be opened. */
int  wb_import_scan_dir(const char *dir, char out[][WB_IMPORT_PATH_MAX], int max);

/* Import an audio file onto the session at timeline position pos_sec.
 * Creates/reuses an audio track; grows session length to cover the clip.
 * Returns 0 on success, -1 on failure (errbuf gets a human-readable reason). */
int  wb_import_audio_file(wb_session *s, const char *path, double pos_sec,
                          char *errbuf, size_t errsz);

/* Index of the track the last successful wb_import_audio_file used (-1 none). */
int  wb_import_last_track(void);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_IMPORT_H */
