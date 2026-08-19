/* wb_captions.c — auto-captions pipeline for the video editor.
 *
 * Phase 1 (R011): whisper-cli subprocess → SRT → ffmpeg burn.
 * Reuses the same whisper.cpp build + model as wb_whisper_test.
 *
 * Pipeline:
 *   1. Extract audio from video: ffmpeg -i VIDEO -vn -ar 16000 -ac 1 audio.wav
 *   2. Transcribe via whisper-cli: whisper-cli -m MODEL -f audio.wav -osrt -otxt
 *   3. Read SRT + TXT output
 *   4. Burn captions on export: ffmpeg -i VIDEO -vf subtitles=out.srt -c:a copy OUT
 */

#include "wbus/wbus_captions.h"
#include "wbus/wbus_transcript.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

/* ---- config (override via -D) */#ifndef WHISPER_CLI
#define WHISPER_CLI  "/Users/waefrebeorn/whisper.cpp/build/bin/whisper-cli"
#endif
#ifndef WHISPER_MODEL
#define WHISPER_MODEL "/Users/waefrebeorn/whisper.cpp/models/ggml-tiny.en-q5_1.bin"
#endif
#ifndef CAPTIONS_TMP_DIR
#define CAPTIONS_TMP_DIR "/tmp/wb_captions"
#endif
/* Full-featured tessus FFmpeg (libavfilter: subtitles burn, etc.). The
 * homebrew minimal build lacks the filters caption burn/extract need. */
#ifndef FFmpeg_BIN
#define FFmpeg_BIN "/Users/waefrebeorn/.local/bin/ffmpeg"
#endif

/* ---- internal state ---------------------------------------------------- */

struct wb_captions {
    char  audio_wav[512];    /* extracted audio path */
    char  srt_path[512];     /* SRT output path */
    char  txt_path[512];     /* transcript text output path */
    char  last_transcript[65536]; /* cached transcript text */
    char  last_srt[65536];   /* cached SRT content */
    int   has_transcript;
    int   has_srt;
    wb_transcript *transcript;  /* lazily parsed word model (G6) */
};

/* ---- helpers ----------------------------------------------------------- */

static int rm_rf(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
    return system(cmd);
}

static int mkdir_p(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", path);
    return system(cmd);
}

/* Run a command, return exit code. Logs to stderr on failure. */
int run_cmd(const char *cmd, const char *context) {
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "wb_captions: %s failed (exit %d): %s\n",
                context, WEXITSTATUS(ret), cmd);
    }
    return WEXITSTATUS(ret);
}

/* Write mono 16-bit PCM as WAV (same as wb_whisper.c). */
static int write_wav(const char *path, const short *pcm, size_t nframes, int sr) {
    if (!pcm || nframes == 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    fwrite("RIFF", 1, 4, f);
    uint32_t chunk_size = 36 + (uint32_t)nframes * 2;
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_fmt = 1, channels = 1, bits = 16;
    uint32_t sample_rate = (uint32_t)sr;
    uint32_t byte_rate = sample_rate * 2;
    uint16_t block_align = 2;
    fwrite(&audio_fmt, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);

    fwrite("data", 1, 4, f);
    uint32_t data_size = (uint32_t)nframes * 2;
    fwrite(&data_size, 4, 1, f);
    fwrite(pcm, 2, nframes, f);
    fclose(f);
    return 0;
}

/* Read entire file into malloc'd buffer. Returns NULL on error. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ---- public API --------------------------------------------------------- */

wb_captions *wb_captions_create(void) {
    wb_captions *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    snprintf(c->srt_path, sizeof(c->srt_path), "%s/%s", CAPTIONS_TMP_DIR, "out.srt");
    snprintf(c->txt_path, sizeof(c->txt_path), "%s/%s", CAPTIONS_TMP_DIR, "out.txt");
    c->has_transcript = 0;
    c->has_srt = 0;
    return c;
}

void wb_captions_free(wb_captions *c) {
    if (!c) return;
    wb_transcript *tr = c->transcript;
    if (tr) wb_transcript_free(tr);
    free(c);
}

/* Generate captions from a video file.
 * Steps:
 *   1. Extract audio to 16kHz mono WAV
 *   2. Run whisper-cli to produce SRT + TXT
 *   3. Read both outputs into cache
 *
 * Returns 0 on success, -1 on error. */
int wb_captions_generate(wb_captions *c, const char *video_path) {
    if (!c || !video_path) return -1;

    /* Step 1: extract audio */
    snprintf(c->audio_wav, sizeof(c->audio_wav), "%s/audio.wav", CAPTIONS_TMP_DIR);
    char extract_cmd[1024];
    snprintf(extract_cmd, sizeof(extract_cmd),
             "\"%s\" -y -i \"%s\" -vn -ar 16000 -ac 1 -c:a pcm_s16le \"%s\" > /dev/null 2>&1",
             FFmpeg_BIN, video_path, c->audio_wav);
    if (run_cmd(extract_cmd, "audio extraction") != 0) return -1;

    /* Verify audio file exists */
    FILE *f = fopen(c->audio_wav, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    if (sz < 44) return -1;  /* WAV header minimum */

    /* Step 2: transcribe via whisper-cli */
    char whisper_cmd[2048];
    snprintf(whisper_cmd, sizeof(whisper_cmd),
             "\"%s\" -m \"%s\" -f \"%s\" -t 4 -nt -osrt -otxt -of \"%s/out\" > /dev/null 2>&1",
             WHISPER_CLI, WHISPER_MODEL, c->audio_wav, CAPTIONS_TMP_DIR);
    if (run_cmd(whisper_cmd, "whisper transcription") != 0) return -1;

    /* Step 3: read outputs */
    char *txt = read_file(c->txt_path);
    if (txt) {
        strncpy(c->last_transcript, txt, sizeof(c->last_transcript) - 1);
        c->last_transcript[sizeof(c->last_transcript) - 1] = '\0';
        /* trim trailing whitespace */
        size_t l = strlen(c->last_transcript);
        while (l > 0 && (c->last_transcript[l-1] == '\n' || c->last_transcript[l-1] == '\r' || c->last_transcript[l-1] == ' '))
            c->last_transcript[--l] = '\0';
        c->has_transcript = 1;
        free(txt);
    }

    char *srt = read_file(c->srt_path);
    if (srt) {
        strncpy(c->last_srt, srt, sizeof(c->last_srt) - 1);
        c->last_srt[sizeof(c->last_srt) - 1] = '\0';
        c->has_srt = 1;
        free(srt);
    }

    return 0;
}

const char *wb_captions_get_transcript(wb_captions *c) {
    if (!c) return NULL;
    return c->has_transcript ? c->last_transcript : NULL;
}

const char *wb_captions_get_srt(wb_captions *c) {
    if (!c) return NULL;
    return c->has_srt ? c->last_srt : NULL;
}

/* G6: lazily parse the generated SRT into an editable word-level transcript
 * model (click-to-seek / drag-to-trim). Returns NULL until captions exist
 * or if the SRT cannot be parsed. Caller does NOT free the returned pointer;
 * it lives as long as the captions context. */
wb_transcript *wb_captions_get_transcript_model(wb_captions *c) {
    if (!c || !c->has_srt) return NULL;
    if (!c->transcript && c->srt_path[0]) {
        c->transcript = wb_transcript_from_srt(c->srt_path);
    }
    return c->transcript;
}

/* Write SRT from transcript text. Splits into segments of roughly equal length.
 * `duration_ms` is the total duration. Returns 0 on success. */
int wb_captions_write_srt(const char *srt_path, const char *text, int duration_ms) {
    if (!srt_path || !text || duration_ms <= 0) return -1;
    FILE *f = fopen(srt_path, "w");
    if (!f) return -1;

    /* Split into segments of ~5 seconds each (or fewer if short) */
    int seg_ms = 5000;
    if (duration_ms < seg_ms) seg_ms = duration_ms;
    int num_segments = duration_ms / seg_ms;
    if (num_segments < 1) num_segments = 1;

    /* Simple word-wrap: split text into roughly equal parts */
    char *text_copy = strdup(text);
    char *words[256];
    int word_count = 0;
    char *tok = strtok(text_copy, " ");
    while (tok && word_count < 255) {
        words[word_count++] = tok;
        tok = strtok(NULL, " ");
    }

    /* Distribute words across segments */
    int words_per_seg = word_count / num_segments;
    if (words_per_seg < 1) words_per_seg = 1;

    for (int i = 0; i < num_segments; i++) {
        int start_ms = i * seg_ms;
        int end_ms = (i + 1) * seg_ms;
        if (end_ms > duration_ms) end_ms = duration_ms;

        /* Build text for this segment */
        char seg_text[4096] = {0};
        int wstart = i * words_per_seg;
        int wend = wstart + words_per_seg;
        if (wend > word_count) wend = word_count;
        for (int w = wstart; w < wend; w++) {
            if (w > wstart) strcat(seg_text, " ");
            strcat(seg_text, words[w]);
        }

        fprintf(f, "%d\n", i + 1);
        fprintf(f, "%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d\n",
                start_ms / 3600000, (start_ms / 60000) % 60,
                (start_ms / 1000) % 60, start_ms % 1000,
                end_ms / 3600000, (end_ms / 60000) % 60,
                (end_ms / 1000) % 60, end_ms % 1000);
        fprintf(f, "%s\n\n", seg_text);
    }
    fclose(f);
    free(text_copy);
    return 0;
}

/* Burn SRT captions into a video file via ffmpeg subtitles filter.
 * Returns 0 on success, -1 on error. */
int wb_captions_burn(const char *input_path, const char *srt_path,
                     const char *output_path) {
    if (!input_path || !srt_path || !output_path) return -1;

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -y -i \"%s\" -vf \"subtitles=%s:force_style='FontSize=24,FontName=Arial,BorderStyle=3,Outline=1,Shadow=0'\" "
             "-c:a copy -c:v libx264 -preset fast -crf 23 \"%s\" > /dev/null 2>&1",
             FFmpeg_BIN, input_path, srt_path, output_path);
    return run_cmd(cmd, "caption burn");
}

/* ---- G10: ASS styled-caption parser + burn --------------------------
 * Parse Dialogue lines from an ASS file. We support the inline override
 * tokens the research flagged (T4-7): \b1/\b0, \i1/\i0, \c&HBBGGRR& (note
 * ASS color is BGR order), \pos(x,y), \move(x1,y1,x2,y2). The burned output
 * is produced by ffmpeg's subtitles filter, which renders full ASS styling. */

static int ass_time_to_ms(const char *s) {
    /* H:MM:SS.cc or 0:00:00.00 */
    int h=0,m=0,sec=0,cc=0;
    if (sscanf(s, "%d:%d:%d.%d", &h,&m,&sec,&cc) == 4) {}
    else if (sscanf(s, "%d:%d.%d", &m,&sec,&cc) == 3) {}
    return ((h*3600 + m*60 + sec)*1000 + cc*10);
}

/* Parse inline overrides in `text`; mutates *bold/*italic/*color (BGR->RGB),
 * *px/*py (if \pos). Strips the override tags from the returned text. */
static void ass_apply_overrides(char *text, int *bold, int *italic,
                                 int *color, int *px, int *py) {
    char *out = text;
    for (char *p = text; *p; ) {
        if (*p == '\\' && (p[1]=='b' || p[1]=='i' || p[1]=='c' || p[1]=='p' || p[1]=='m')) {
            if (p[1]=='b') { *bold = (p[2]=='1') ? 1 : 0; p += 3; continue; }
            if (p[1]=='i') { *italic = (p[2]=='1') ? 1 : 0; p += 3; continue; }
            if (p[1]=='c' && p[2]=='&' && p[3]=='H') {
                unsigned int bgr=0; int n=0;
                sscanf(p+4, "%x%n", &bgr, &n);
                int bb=(bgr>>16)&0xff, gg=(bgr>>8)&0xff, rr=bgr&0xff;
                *color = (rr<<16)|(gg<<8)|bb;   /* convert BGR -> RGB */
                p += 4 + n + 1; /* skip &H......& */
                continue;
            }
            if (p[1]=='p' && p[2]=='o' && p[3]=='s') {
                int x=0,y=0; sscanf(p+4, "(%d,%d)", &x,&y);
                *px=x; *py=y; p += 4; while (*p && *p!=')') p++; if (*p==')') p++;
                continue;
            }
            if (p[1]=='m' && p[2]=='o' && p[3]=='v' && p[4]=='e') {
                int x1=0,y1=0,x2=0,y2=0;
                sscanf(p+5, "(%d,%d,%d,%d)", &x1,&y1,&x2,&y2);
                *px=x1; *py=y1; p += 5; while (*p && *p!=')') p++; if (*p==')') p++;
                continue;
            }
        }
        *out++ = *p++;
    }
    *out = '\0';
}

int wb_ass_extract_dialogue(const char *ass_path, wb_ass_line *out, int max) {
    if (!ass_path || !out || max <= 0) return -1;
    char *buf = read_file(ass_path);
    if (!buf) return -1;
    int n = 0;
    char *save_line = NULL;
    char *line = strtok_r(buf, "\n", &save_line);
    while (line && n < max) {
        char *l = line; while (*l==' '||*l=='\r') l++;
        if (strncmp(l, "Dialogue:", 9) == 0) {
            char *p = l + 9; if (*p==':') p++;
            while (*p==' ') p++;
            /* ASS: 9 metadata fields (Layer,Start,End,Style,Name,MarginL,
             * MarginR,MarginV,Effect), then Text is EVERYTHING after the
             * 9th comma — so we must NOT split the text further (it may
             * contain commas inside overrides like \pos(100,200)). We locate
             * the comma positions directly and take the tail as text. */
            int comma[12]; int nc = 0;
            for (char *q = p; *q && nc < 12; q++) if (*q == ',') comma[nc++] = (int)(q - p);
            if (nc >= 3) {
                /* start = field[1] (between comma[0] and comma[1]) */
                char t0[32], t1[32];
                int a = (nc >= 1) ? comma[0] + 1 : 0;
                int b = (nc >= 2) ? comma[1] : (int)strlen(p);
                snprintf(t0, sizeof(t0), "%.*s", b - a, p + a);
                a = comma[1] + 1; b = (nc >= 3) ? comma[2] : (int)strlen(p);
                snprintf(t1, sizeof(t1), "%.*s", b - a, p + a);
                wb_ass_line *L = &out[n];
                L->start_ms = ass_time_to_ms(t0);
                L->end_ms   = ass_time_to_ms(t1);
                L->bold = 0; L->italic = 0; L->color_rgb = -1;
                L->pos_x = -1; L->pos_y = -1;
                /* text = everything after the 9th comma (comma[8]) */
                const char *txt = (nc >= 9) ? p + comma[8] + 1 : p + comma[nc-1] + 1;
                strncpy(L->text, txt ? txt : "", sizeof(L->text)-1);
                L->text[sizeof(L->text)-1] = '\0';
                ass_apply_overrides(L->text, &L->bold, &L->italic,
                                    &L->color_rgb, &L->pos_x, &L->pos_y);
                n++;
            }
        }
        line = strtok_r(NULL, "\n", &save_line);
    }
    free(buf);
    return n;
}

int wb_captions_burn_ass(const char *input_path, const char *ass_path,
                         const char *output_path) {
    if (!input_path || !ass_path || !output_path) return -1;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -y -i \"%s\" -vf \"subtitles=%s\" "
             "-c:a copy -c:v libx264 -preset fast -crf 23 \"%s\" > /dev/null 2>&1",
             FFmpeg_BIN, input_path, ass_path, output_path);
    return run_cmd(cmd, "ASS caption burn");
}

/* Clean up temporary files. */
void wb_captions_cleanup(wb_captions *c) {
    if (!c) return;
    rm_rf(CAPTIONS_TMP_DIR);
    c->has_transcript = 0;
    c->has_srt = 0;
    c->last_transcript[0] = '\0';
    c->last_srt[0] = '\0';
}

/* ---- flat wrappers for video tab shortcuts (no context needed) ---- */

int wb_video_captions_generate(const char *video_path, const char *srt_out_path,
                                const char *cli_path, const char *model_path) {
    (void)srt_out_path; (void)cli_path; (void)model_path;
    wb_captions *c = wb_captions_create();
    if (!c) return -1;
    int rc = wb_captions_generate(c, video_path);
    if (rc == 0) {
        const char *srt = wb_captions_get_srt(c);
        if (srt) {
            FILE *f = fopen(srt_out_path, "w");
            if (f) { fputs(srt, f); fclose(f); }
        }
    }
    wb_captions_free(c);
    return rc;
}

int wb_video_captions_burn(const char *input_path, const char *output_path,
                            const char *srt_path, const char *ffmpeg_path) {
    (void)ffmpeg_path;
    return wb_captions_burn(input_path, srt_path, output_path);
}
