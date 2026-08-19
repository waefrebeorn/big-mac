/* wb_agent.c — headless agent command processor (R017 G9). Pure C11.
 * Drives the editor via the real session/export/EDL/voice-polish APIs. */

#include "wbus/wbus_agent.h"
#include "wbus/wbus_video.h"
#include "wbus/wbus_compositor.h"
#include "wbus/wbus_voice_polish.h"
#include "wbus/wbus_captions.h"
#include "wb_internal.h"   /* wb_wav_read/write_pcm16 (internal) */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\n' || s[n-1] == '\r'))
        s[--n] = '\0';
    return s;
}

/* Read a quoted-or-unquoted token; advances *pp past it. Returns a pointer
 * into a small rotating buffer (so up to 4 tokens stay live simultaneously —
 * do NOT free; the next 4 calls overwrite them). */
static char *tok(char **pp) {
    static char buf[4][1024];
    static int slot = 0;
    char *dst = buf[slot];
    slot = (slot + 1) & 3;
    char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < 1023) dst[i++] = *p++;
        if (*p == '"') p++;
        dst[i] = '\0';
    } else {
        int i = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && i < 1023)
            dst[i++] = *p++;
        dst[i] = '\0';
    }
    *pp = p;
    return dst;
}

static int do_import(wb_session *s, const char *src) {
    int vt = -1;
    for (uint32_t t = 0; t < s->track_count; t++)
        if (s->tracks[t].kind == WB_TRACK_KIND_VIDEO) { vt = (int)t; break; }
    if (vt < 0) vt = wb_session_add_video_track(s, "V1");
    int clip = wb_session_add_video_clip(s, vt, src, 0.0);
    return clip >= 0 ? 0 : -1;
}

int wb_agent_command(wb_session *s, wb_engine *e, const char *line) {
    char buf[2048];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *p = trim(buf);
    if (*p == '\0' || *p == '#') return 0;
    char *cmd = tok(&p);

    if (strcmp(cmd, "import") == 0) {
        char *src = tok(&p);
        return do_import(s, src);
    }
    if (strcmp(cmd, "split") == 0) {
        int track = atoi(tok(&p));
        int clip  = atoi(tok(&p));
        double t  = atof(tok(&p));
        return wb_session_split_video_clip(s, track, clip, t) >= 0 ? 0 : -1;
    }
    if (strcmp(cmd, "quality") == 0) {
        double q = atof(tok(&p));
        wb_compositor_set_quality(q);
        return 0;
    }
    if (strcmp(cmd, "edl") == 0) {
        return wb_session_export_edl(s, tok(&p));
    }
    if (strcmp(cmd, "fcpxml") == 0) {
        return wb_session_export_fcpxml(s, tok(&p));
    }
    if (strcmp(cmd, "export") == 0) {
        char *out = tok(&p);
        char *srt = tok(&p);
        if (!srt || *srt == '\0') srt = NULL;
        return wb_video_export(s, e, out, srt);
    }
    if (strcmp(cmd, "polish") == 0) {
        /* polish <src.wav> <out.wav> <lufs> : two-pass voice polish (G8) */
        char *src = tok(&p);
        char *out = tok(&p);
        double lufs = atof(tok(&p));
        /* load wav, polish, write */
        int sr = 0, ch = 0; uint32_t frames = 0;
        float *data = NULL;
        if (wb_wav_read_pcm16(src, &data, &frames, &ch, &sr) != 0 || !data)
            return -1;
        wb_voice_polish_apply_twopass(data, frames, ch, (float)sr, (float)lufs);
        int rc = wb_wav_write_pcm16(out, data, frames, ch, sr);
        free(data);
        return rc;
    }
    if (strcmp(cmd, "quit") == 0) return 0;
    fprintf(stderr, "wb_agent: unknown command '%s'\n", cmd);
    return -1;
}

int wb_agent_run(FILE *in, wb_session *s, wb_engine *e) {
    if (!in || !s || !e) return -1;
    char line[2048];
    int rc = 0;
    while (fgets(line, sizeof(line), in)) {
        if (wb_agent_command(s, e, line) != 0) rc = -1;
    }
    return rc;
}
