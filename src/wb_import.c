/* wb_import.c — Wave 1 G01/G02: media scan + audio-file import.
 *
 * Pure C11 (POSIX dirent for the directory scan; system() shells the full
 * ffmpeg binary for compressed formats, exactly like wb_captions/wb_agent).
 * No third-party libraries linked.
 */

#include "wbus_import.h"
#include "wb_internal.h"   /* wb_wav_read_pcm16 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#define WB_IMPORT_FFMPEG "/Users/waefrebeorn/.local/bin/ffmpeg"

static int ext_is(const char *path, const char *ext) {
    size_t pl = strlen(path), el = strlen(ext);
    return pl > el && strcasecmp(path + pl - el, ext) == 0;
}

int wb_import_is_media_path(const char *path) {
    if (!path) return 0;
    return ext_is(path, ".mp4") || ext_is(path, ".mov") || ext_is(path, ".wav") ||
           ext_is(path, ".aiff") || ext_is(path, ".mp3") || ext_is(path, ".m4a");
}

static int cmp_paths(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

int wb_import_scan_dir(const char *dir, char out[][WB_IMPORT_PATH_MAX], int max) {
    if (!dir || !out || max <= 0) return -1;
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (e->d_name[0] == '.') continue;
        char full[WB_IMPORT_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!wb_import_is_media_path(e->d_name)) continue;
        snprintf(out[n], WB_IMPORT_PATH_MAX, "%s", full);
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, WB_IMPORT_PATH_MAX, cmp_paths);
    return n;
}

static int last_track = -1;
int wb_import_last_track(void) { return last_track; }

/* Find an existing AUDIO track with room for another clip, else create one.
 * Returns track index or -1. */
static int ensure_audio_track(wb_session *s) {
    for (uint32_t ti = 0; ti < s->track_count; ti++)
        if (s->tracks[ti].kind == WB_TRACK_KIND_AUDIO)
            return (int)ti;
    if (s->track_count >= WB_MAX_TRACKS) return -1;
    wb_track *tr = wb_session_add_track(s, "Audio", WB_TRACK_KIND_AUDIO);
    return tr ? (int)(s->track_count - 1) : -1;
}

int wb_import_audio_file(wb_session *s, const char *path, double pos_sec,
                         char *errbuf, size_t errsz) {
    last_track = -1;
    if (!s || !path || !path[0]) { snprintf(errbuf, errsz, "bad arguments"); return -1; }
    if (access(path, R_OK) != 0) { snprintf(errbuf, errsz, "cannot read %s", path); return -1; }

    char tmp[512];
    const char *src = path;
    if (!ext_is(path, ".wav")) {
        /* transcode via ffmpeg -> pcm_s16le wav at engine rate */
        snprintf(tmp, sizeof(tmp), "/tmp/wb_imp_%d.wav", (int)getpid());
        char cmd[1400];
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -y -i \"%s\" -vn -acodec pcm_s16le -ar %d \"%s\" > /dev/null 2>&1",
                 WB_IMPORT_FFMPEG, path, WB_SAMPLE_RATE, tmp);
        if (system(cmd) != 0 || access(tmp, R_OK) != 0) {
            snprintf(errbuf, errsz, "ffmpeg transcode failed for %s", path);
            return -1;
        }
        src = tmp;
    }

    float *data = NULL;
    uint32_t frames = 0;
    int ch = 0, sr = 0;
    if (wb_wav_read_pcm16(src, &data, &frames, &ch, &sr) != 0 || !data || frames == 0) {
        snprintf(errbuf, errsz, "wav decode failed for %s", src);
        return -1;
    }

    int ti = ensure_audio_track(s);
    if (ti < 0) { free(data); snprintf(errbuf, errsz, "no audio track available"); return -1; }
    wb_track *tr = &s->tracks[ti];

    double length = (double)frames / (sr > 0 ? sr : WB_SAMPLE_RATE);
    int ci = wb_session_add_audio_clip(tr, pos_sec, length, data, frames, ch);
    free(data);
    if (ci < 0) { snprintf(errbuf, errsz, "session rejected clip"); return -1; }

    /* session length is SAMPLES (see wbus.h) — grow to cover the clip */
    double end_samp = (pos_sec + length) * WB_SAMPLE_RATE;
    if (end_samp > (double)s->length) s->length = end_samp;

    last_track = ti;
    if (src != path) unlink(tmp);
    return 0;
}
