/* wb_stt_caption.c — AI auto-caption / speech-to-text subtitle burn-in.
 *
 * Transcribes audio from a video clip via whisper.cpp CLI, captures SRT
 * output, and burns subtitles onto video frames using the existing text
 * rasterizer (wb_ui_text_to_rgba). Integrates with the edit graph so the
 * agent can drive auto-captioning headlessly.
 *
 * Pipeline:
 *   1. Extract 16kHz mono WAV from the clip's source via ffmpeg.
 *   2. Run whisper-cli → SRT.
 *   3. Parse SRT into caption events (start_ms, end_ms, text).
 *   4. Burn onto frames via wb_ui_text_to_rgba at render time, or
 *      write SRT to disk for ffmpeg-based export burn.
 *
 * C11. Shells out to whisper.cpp — no libwhisper linking needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <math.h>

#include "wbus/wbus_compositor.h"
#include "wbus/wbus_edit.h"
#include "wbus/wbus_captions.h"
#include "wbus/wb_ui.h"

/* ---- config (override via -D) */
#ifndef WHISPER_CLI
#define WHISPER_CLI  "/Users/waefrebeorn/whisper.cpp/build/bin/whisper-cli"
#endif
#ifndef WHISPER_MODEL
#define WHISPER_MODEL "/Users/waefrebeorn/whisper.cpp/models/ggml-tiny.en-q5_1.bin"
#endif
#ifndef STT_TMP_DIR
#define STT_TMP_DIR "/tmp/wb_stt"
#endif
#ifndef STT_FFMPEG
#define STT_FFMPEG "/Users/waefrebeorn/.local/bin/ffmpeg"
#endif

/* ---- internal state -------------------------------------------------- */

#define MAX_SRT_ENTRIES 512
#define SRT_TEXT_SZ     65536

typedef struct {
    int   index;
    int   start_ms;
    int   end_ms;
    char  text[1024];
} srt_entry;

static struct {
    char      language[16];       /* "en", "es", etc. */
    char      last_srt[SRT_TEXT_SZ];
    int       has_result;

    /* parsed SRT entries */
    srt_entry entries[MAX_SRT_ENTRIES];
    int       entry_count;

    /* burn style */
    float     font_scale;         /* text scale (1.0 = base) */
    float     pos_x, pos_y;       /* normalized 0..1 */
    uint32_t  color;              /* RGBA */
    int       enabled;
} g_stt = {
    .language = "en",
    .has_result = 0,
    .entry_count = 0,
    .font_scale = 2.0f,
    .pos_x = 0.5f,
    .pos_y = 0.85f,
    .color = 0xFFFFFFAA,
    .enabled = 1,
};

/* ---- helpers --------------------------------------------------------- */

static int stt_run_cmd(const char *cmd) {
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "wb_stt: command failed (exit %d): %s\n",
                WEXITSTATUS(ret), cmd);
    }
    return WEXITSTATUS(ret);
}

static int stt_mkdir_p(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", STT_TMP_DIR);
    return system(cmd);
}

static char *stt_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len >= SRT_TEXT_SZ) { fclose(f); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Parse "HH:MM:SS,mmm" to milliseconds */
static int srt_parse_time(const char *s) {
    int h = 0, m = 0, sec = 0, ms = 0;
    if (sscanf(s, "%d:%d:%d,%d", &h, &m, &sec, &ms) == 4)
        return ((h * 3600) + (m * 60) + sec) * 1000 + ms;
    return -1;
}

/* Parse SRT content into g_stt.entries. Returns count or -1. */
static int srt_parse(const char *srt) {
    g_stt.entry_count = 0;
    if (!srt || !srt[0]) return -1;

    const char *p = srt;
    while (*p && g_stt.entry_count < MAX_SRT_ENTRIES) {
        /* skip blank lines */
        while (*p == '\n' || *p == '\r') p++;
        if (!*p) break;

        /* index line (optional — may be missing in some outputs) */
        int idx = 0;
        if (sscanf(p, "%d", &idx) == 1) {
            /* consume index line */
            while (*p && *p != '\n' && *p != '\r') p++;
            while (*p == '\n' || *p == '\r') p++;
        }

        /* timestamp line: "HH:MM:SS,mmm --> HH:MM:SS,mmm" */
        char t0[32] = {0}, t1[32] = {0};
        /* read up to '-->' for start */
        int i = 0;
        while (*p && *p != '-' && i < 30) t0[i++] = *p++;
        t0[i] = '\0';
        /* skip " --> " */
        while (*p && *p != '\n' && *p != '\r') {
            if (*p == '-' && p[1] == '-' && p[2] == '>' ) { p += 3; break; }
            p++;
        }
        while (*p == ' ') p++;
        /* read end time */
        i = 0;
        while (*p && *p != '\n' && *p != '\r' && *p != ' ' && i < 30) t1[i++] = *p++;
        t1[i] = '\0';
        while (*p == ' ') p++;
        while (*p == '\n' || *p == '\r') p++;

        int start_ms = srt_parse_time(t0);
        int end_ms   = srt_parse_time(t1);
        if (start_ms < 0 || end_ms < 0) {
            /* malformed — skip this block */
            while (*p && !(p[0] == '\n' && (p[1] == '\n' || p[1] == '\0'))) p++;
            continue;
        }

        /* text lines until blank line */
        char text[1024] = {0};
        int ti = 0;
        while (*p && ti < 1020) {
            if (*p == '\n' && (p[1] == '\n' || p[1] == '\0' || p[1] == '\r')) break;
            if (*p == '\n' || *p == '\r') {
                /* multi-line caption: join with space */
                if (ti > 0 && text[ti-1] != ' ') text[ti++] = ' ';
                p++;
                continue;
            }
            text[ti++] = *p++;
        }
        text[ti] = '\0';
        /* trim trailing whitespace */
        while (ti > 0 && (text[ti-1] == ' ' || text[ti-1] == '\n' || text[ti-1] == '\r'))
            text[--ti] = '\0';

        srt_entry *e = &g_stt.entries[g_stt.entry_count++];
        e->index = (idx > 0) ? idx : g_stt.entry_count;
        e->start_ms = start_ms;
        e->end_ms = end_ms;
        strncpy(e->text, text, sizeof(e->text) - 1);
        e->text[sizeof(e->text) - 1] = '\0';

        /* skip blank line separator */
        while (*p == '\n' || *p == '\r') p++;
    }

    return g_stt.entry_count;
}

/* ---- public API ------------------------------------------------------ */

/* Set the language for STT transcription.
 * lang: ISO-639-1 code ("en", "es", "fr", etc.). Pass "auto" for
 * whisper auto-detection. Returns 0 on success, -1 on bad input. */
int wb_stt_set_language(const char *lang) {
    if (!lang || !lang[0]) return -1;
    strncpy(g_stt.language, lang, sizeof(g_stt.language) - 1);
    g_stt.language[sizeof(g_stt.language) - 1] = '\0';
    return 0;
}

/* Transcribe the audio of a video clip in the edit graph.
 * Extracts audio to WAV, runs whisper-cli, captures + caches SRT.
 * Returns 0 on success, -1 on error. */
int wb_stt_process_audio(wb_edit_graph *g, int track, int clip_idx) {
    if (!g) return -1;
    if (track < 0 || (uint32_t)track >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track];
    if (clip_idx < 0 || (uint32_t)clip_idx >= tr->clip_count) return -1;

    wb_edit_clip *clip = &tr->clips[clip_idx];
    if (!clip->source_path[0]) return -1;

    /* Step 1: extract 16kHz mono WAV via ffmpeg */
    stt_mkdir_p();
    char wav_path[1024];
    snprintf(wav_path, sizeof(wav_path), "%s/stt_audio.wav", STT_TMP_DIR);

    char extract_cmd[2048];
    snprintf(extract_cmd, sizeof(extract_cmd),
             "\"%s\" -y -i \"%s\" -vn -ar 16000 -ac 1 -c:a pcm_s16le \"%s\" > /dev/null 2>&1",
             STT_FFMPEG, clip->source_path, wav_path);
    if (stt_run_cmd(extract_cmd) != 0) {
        fprintf(stderr, "wb_stt: audio extraction failed for %s\n", clip->source_path);
        return -1;
    }

    /* Verify WAV exists */
    FILE *f = fopen(wav_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    if (sz < 44) return -1;

    /* Step 2: run whisper-cli → SRT */
    char srt_path[1024];
    snprintf(srt_path, sizeof(srt_path), "%s/stt_out.srt", STT_TMP_DIR);

    char whisper_cmd[4096];
    int n = snprintf(whisper_cmd, sizeof(whisper_cmd),
                     "\"%s\" -m \"%s\" -f \"%s\" -t 4 -nt -osrt -otxt -of \"%s/stt_out\"",
                     WHISPER_CLI, WHISPER_MODEL, wav_path, STT_TMP_DIR);
    /* append language flag if not auto */
    if (n > 0 && n < (int)sizeof(whisper_cmd) - 32) {
        if (strcmp(g_stt.language, "auto") != 0) {
            snprintf(whisper_cmd + n, sizeof(whisper_cmd) - n,
                     " -l %s", g_stt.language);
        }
    }
    strncat(whisper_cmd, " > /dev/null 2>&1", sizeof(whisper_cmd) - strlen(whisper_cmd) - 1);

    if (stt_run_cmd(whisper_cmd) != 0) {
        fprintf(stderr, "wb_stt: whisper transcription failed\n");
        return -1;
    }

    /* Step 3: read SRT output */
    char *srt = stt_read_file(srt_path);
    if (!srt) {
        fprintf(stderr, "wb_stt: could not read SRT output\n");
        return -1;
    }

    strncpy(g_stt.last_srt, srt, SRT_TEXT_SZ - 1);
    g_stt.last_srt[SRT_TEXT_SZ - 1] = '\0';
    g_stt.has_result = 1;
    free(srt);

    /* Step 4: parse into entries */
    int count = srt_parse(g_stt.last_srt);
    if (count <= 0) {
        fprintf(stderr, "wb_stt: SRT parsed to 0 entries\n");
        return -1;
    }

    return 0;
}

/* Get the last transcription result as SRT text.
 * Copies into buf (bufsize bytes). Returns bytes written, or -1 if none. */
int wb_stt_get_transcription_result(char *buf, int bufsize) {
    if (!buf || bufsize <= 0) return -1;
    if (!g_stt.has_result) { buf[0] = '\0'; return -1; }
    strncpy(buf, g_stt.last_srt, (size_t)bufsize - 1);
    buf[bufsize - 1] = '\0';
    return (int)strlen(buf);
}

/* Burn SRT subtitles onto a video clip's frames.
 * Parses the SRT string, then for each frame in the clip's time range,
 * overlays the active caption text using wb_ui_text_to_rgba.
 *
 * This operates on the edit graph's subtitle overlay: it sets the
 * subtitle text on the graph so the compositor picks it up during
 * export. For per-frame burn, it writes the SRT to disk and the
 * export path uses ffmpeg subtitles filter.
 *
 * Returns 0 on success, -1 on error. */
int wb_stt_burn_subtitles(wb_edit_graph *g, int track, int clip_idx, const char *srt) {
    (void)track; (void)clip_idx;
    if (!g || !srt || !srt[0]) return -1;

    /* Parse the SRT to validate it */
    int count = srt_parse(srt);
    if (count <= 0) return -1;

    /* Write SRT to disk for ffmpeg-based export burn */
    char srt_path[1024];
    snprintf(srt_path, sizeof(srt_path), "%s/stt_burn.srt", STT_TMP_DIR);
    FILE *f = fopen(srt_path, "w");
    if (!f) return -1;
    fputs(srt, f);
    fclose(f);

    /* Cache the SRT in our internal state too */
    strncpy(g_stt.last_srt, srt, SRT_TEXT_SZ - 1);
    g_stt.last_srt[SRT_TEXT_SZ - 1] = '\0';
    g_stt.has_result = 1;

    /* Set the first entry as the edit graph's subtitle overlay.
     * The compositor's export path reads subtitle_text and burns it.
     * For multi-entry SRT, the export path uses the SRT file directly. */
    if (g_stt.entry_count > 0) {
        wb_edit_set_subtitle(g, g_stt.entries[0].text);
    }

    return 0;
}

/* Get the number of parsed SRT entries from the last transcription. */
int wb_stt_get_entry_count(void) {
    return g_stt.entry_count;
}

/* Get a parsed SRT entry by index. Returns NULL if out of range. */
const srt_entry *wb_stt_get_entry(int idx) {
    if (idx < 0 || idx >= g_stt.entry_count) return NULL;
    return &g_stt.entries[idx];
}

/* Find the active caption text for a given time (ms).
 * Returns the text of the entry containing `time_ms`, or "" if none. */
const char *wb_stt_get_caption_at(int time_ms) {
    for (int i = 0; i < g_stt.entry_count; i++) {
        if (time_ms >= g_stt.entries[i].start_ms && time_ms < g_stt.entries[i].end_ms)
            return g_stt.entries[i].text;
    }
    return "";
}

/* Configure subtitle burn style.
 * scale: font scale (1.0 = base). x, y: normalized position (0..1).
 * color: RGBA 8-bit color. */
void wb_stt_set_style(float scale, float x, float y, uint32_t color) {
    g_stt.font_scale = scale > 0 ? scale : 2.0f;
    g_stt.pos_x = x < 0 ? 0 : (x > 1 ? 1 : x);
    g_stt.pos_y = y < 0 ? 0 : (y > 1 ? 1 : y);
    g_stt.color = color;
}

/* Enable/disable subtitle overlay. */
void wb_stt_set_enabled(int enabled) {
    g_stt.enabled = enabled ? 1 : 0;
}

/* Burn subtitles directly onto a frame buffer using wb_ui_text_to_rgba.
 * Call this during frame compositing to overlay the active caption at
 * time `time_ms` onto the frame. Respects g_stt.enabled and style. */
void wb_stt_burn_on_frame(wb_frame *f, int time_ms) {
    if (!f || !g_stt.enabled || g_stt.entry_count == 0) return;

    const char *text = wb_stt_get_caption_at(time_ms);
    if (!text || !text[0]) return;

    /* Decode color */
    float cr = ((g_stt.color >> 24) & 0xFF) / 255.0f;
    float cg = ((g_stt.color >> 16) & 0xFF) / 255.0f;
    float cb = ((g_stt.color >> 8) & 0xFF) / 255.0f;
    float ca = (g_stt.color & 0xFF) / 255.0f;

    /* Position: centered horizontally, pos_y from bottom */
    int x0 = (int)(f->w * g_stt.pos_x);
    int y0 = (int)(f->h * g_stt.pos_y);

    /* Estimate text width for centering (approx: 6px per char * scale) */
    int approx_w = (int)(strlen(text) * 6 * g_stt.font_scale);
    x0 -= approx_w / 2;
    if (x0 < 8) x0 = 8;

    /* Draw background box for readability */
    int box_h = (int)(9 * g_stt.font_scale + 8);
    int box_w = approx_w + 16;
    int box_x = x0 - 8;
    int box_y = y0 - 4;
    for (int py = box_y; py < box_y + box_h && py < f->h; py++) {
        if (py < 0) continue;
        for (int px = box_x; px < box_x + box_w && px < f->w; px++) {
            if (px < 0) continue;
            wb_px *q; /* placeholder — actual blend below */
            q = &f->px[py * f->w + px];
            float ia = 0.6f;
            q->r = 0.0f * 0.6f + q->r * ia;
            q->g = 0.0f * 0.6f + q->g * ia;
            q->b = 0.0f * 0.6f + q->b * ia;
            q->a = 0.6f + q->a * (1.0f - 0.6f);
        }
    }

    /* Render text */
    wb_ui_text_to_rgba(text, (int)g_stt.font_scale, cr, cg, cb, ca,
                       f->px, f->w, f->h, x0, y0);
}

/* Get the path to the last written SRT file (for ffmpeg export burn). */
const char *wb_stt_get_srt_path(void) {
    static char path[1024];
    snprintf(path, sizeof(path), "%s/stt_burn.srt", STT_TMP_DIR);
    return path;
}