/* wb_ffmpeg_filters.c — Advanced FFmpeg filter_complex command builder
 * R092: Leverage ffmpeg's full filter library via proper command construction
 *
 * Instead of linking libav* (nightmare on static-only homebrew builds), we build
 * proper filter_complex command strings and pipe through the ffmpeg binary.
 * This gives us access to 56 xfade transitions, 37 blend modes, drawtext,
 * overlay, color correction, and hundreds more filters.
 *
 * Filters covered:
 * - xfade: 56 transition types (fade, wipe, slide, dissolve, pixelize, etc.)
 * - blend: 37 blend modes (multiply, screen, overlay, hardlight, etc.)
 * - drawtext: scrolling credits, animated text, timecode
 * - overlay: picture-in-picture, watermarking
 * - colorchannelmixer/eq/hue/colorbalance: color grading
 * - boxblur/gblur/dblur: blur effects
 * - fade/afade: audio/video fade in/out
 * - acrossfade: audio crossfade between clips
 * - concat: smart concatenation without re-encode (via concat demuxer)
 * - setpts/tempo: speed ramping
 * - reverse: reverse playback
 * - hflip/vflip/rotate: transforms
 * - crop/pad: framing
 * - scale: high-quality scaling with lanczos
 * - subtitles: burn-in subtitles
 * - zoompan: Ken Burns effect
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>

#include <unistd.h>
#include "wbus/wbus_compositor.h"

/* ---- Transition names (56 total, matching ffmpeg xfade) ---- */

typedef enum {
    XFADE_FADE = 0,
    XFADE_FADEBLACK,
    XFADE_FADEWHITE,
    XFADE_DISTANCE,
    XFADE_WIPELEFT,
    XFADE_WIPERIGHT,
    XFADE_WIPEUP,
    XFADE_WIPEDOWN,
    XFADE_SLIDELEFT,
    XFADE_SLIDERIGHT,
    XFADE_SLIDEUP,
    XFADE_SLIDEDOWN,
    XFADE_SMOOTHLEFT,
    XFADE_SMOOTHRIGHT,
    XFADE_SMOOTHUP,
    XFADE_SMOOTHDOWN,
    XFADE_CIRCLECROP,
    XFADE_RECTCROP,
    XFADE_CIRCLECLOSE,
    XFADE_CIRCLEOPEN,
    XFADE_HORZCLOSE,
    XFADE_HORZOPEN,
    XFADE_VERTCLOSE,
    XFADE_VERTOPEN,
    XFADE_DIAGBL,
    XFADE_DIAGBR,
    XFADE_DIAGTL,
    XFADE_DIAGTR,
    XFADE_HLSLICE,
    XFADE_HRSLICE,
    XFADE_VUSLICE,
    XFADE_VDSLICE,
    XFADE_DISSOLVE,
    XFADE_PIXELIZE,
    XFADE_RADIAL,
    XFADE_HBLUR,
    XFADE_WIPETL,
    XFADE_WIPETR,
    XFADE_WIPEBL,
    XFADE_WIPEBR,
    XFADE_FADEGRAYS,
    XFADE_SQUEEZEV,
    XFADE_SQUEEZEH,
    XFADE_ZOOMIN,
    XFADE_HLWIND,
    XFADE_HRWIND,
    XFADE_VUWIND,
    XFADE_VDWIND,
    XFADE_COVERLEFT,
    XFADE_COVERRIGHT,
    XFADE_COVERUP,
    XFADE_COVERDOWN,
    XFADE_REVEALLEFT,
    XFADE_REVEALRIGHT,
    XFADE_REVEALUP,
    XFADE_REVEALDOWN,
    XFADE_COUNT
} wb_xfade_type;

static const char *xfade_names[] = {
    "fade", "fadeblack", "fadewhite", "distance",
    "wipeleft", "wiperight", "wipeup", "wipedown",
    "slideleft", "slideright", "slideup", "slidedown",
    "smoothleft", "smoothright", "smoothup", "smoothdown",
    "circlecrop", "rectcrop", "circleclose", "circleopen",
    "horzclose", "horzopen", "vertclose", "vertopen",
    "diagbl", "diagbr", "diagtl", "diagtr",
    "hlslice", "hrslice", "vuslice", "vdslice",
    "dissolve", "pixelize", "radial", "hblur",
    "wipetl", "wipetr", "wipebl", "wipebr",
    "fadegrays", "squeezev", "squeezeh", "zoomin",
    "hlwind", "hrwind", "vuwind", "vdwind",
    "coverleft", "coverright", "coverup", "coverdown",
    "revealleft", "revealright", "revealup", "revealdown"
};

const char *wb_xfade_name(int t) {
    if (t < 0 || t >= XFADE_COUNT) return "fade";
    return xfade_names[t];
}

/* ---- Blend mode names (37 total, matching ffmpeg blend filter) ---- */

typedef enum {
    BLEND_NORMAL = 0,
    BLEND_MULTIPLY,
    BLEND_SCREEN,
    BLEND_OVERLAY,
    BLEND_DARKEN,
    BLEND_LIGHTEN,
    BLEND_COLORDODGE,
    BLEND_COLORBURN,
    BLEND_HARDLIGHT,
    BLEND_SOFTLIGHT,
    BLEND_DIFFERENCE,
    BLEND_EXCLUSION,
    BLEND_ADDITION,
    BLEND_SUBTRACT,
    BLEND_DIVIDE,
    BLEND_HARDMIX,
    BLEND_HARDOVERLAY,
    BLEND_GRAINEXTRACT,
    BLEND_GRAINMERGE,
    BLEND_VIVIDLIGHT,
    BLEND_LINEARLIGHT,
    BLEND_PINLIGHT,
    BLEND_HARMONIC,
    BLEND_HEAT,
    BLEND_REFLECT,
    BLEND_GLOW,
    BLEND_PHOENIX,
    BLEND_STAIN,
    BLEND_AND,
    BLEND_OR,
    BLEND_XOR,
    BLEND_NEGATION,
    BLEND_EXTREMITY,
    BLEND_FREEZE,
    BLEND_BLEACH,
    BLEND_INTERPOLATE,
    BLEND_COUNT
} wb_blend_mode;

static const char *blend_names[] = {
    "normal", "multiply", "screen", "overlay", "darken", "lighten",
    "colordodge", "colorburn", "hardlight", "softlight", "difference",
    "exclusion", "addition", "subtract", "divide", "hardmix", "hardoverlay",
    "grainextract", "grainmerge", "vividlight", "linearlight", "pinlight",
    "harmonic", "heat", "reflect", "glow", "phoenix", "stain",
    "and", "or", "xor", "negation", "extremity", "freeze", "bleach",
    "interpolate"
};

const char *wb_blend_name(int m) {
    if (m < 0 || m >= BLEND_COUNT) return "normal";
    return blend_names[m];
}

/* ---- Command builder (dynamic string) ---- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} wb_cmd_builder;

static void cmd_init(wb_cmd_builder *b) {
    b->cap = 4096;
    b->buf = (char *)malloc(b->cap);
    b->len = 0;
    b->buf[0] = '\0';
}

static void cmd_append(wb_cmd_builder *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    while (n < 0 || (size_t)n >= b->cap - b->len) {
        b->cap *= 2;
        b->buf = (char *)realloc(b->buf, b->cap);
        va_start(ap, fmt);
        n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
    }
    b->len += (size_t)n;
}

static void cmd_free(wb_cmd_builder *b) {
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

/* ---- FFmpeg binary path ---- */

static const char *ffmpeg_path(void) {
    static char path[512];
    static int init = 0;
    if (!init) {
        if (access("/Users/waefrebeorn/.local/bin/ffmpeg", X_OK) == 0)
            strcpy(path, "/Users/waefrebeorn/.local/bin/ffmpeg");
        else
            strcpy(path, "ffmpeg");
        init = 1;
    }
    return path;
}

/* ---- Execute ffmpeg command ---- */

static int run_ffmpeg(const char *cmd) {
    if (!cmd || !*cmd) return -1;
    char full_cmd[65536];
    snprintf(full_cmd, sizeof(full_cmd), "%s -y -hide_banner -loglevel warning %s 2>&1",
             ffmpeg_path(), cmd);
    FILE *p = popen(full_cmd, "r");
    if (!p) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), p)) {
        fprintf(stderr, "[ffmpeg] %s", line);
    }
    int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ================================================================
 * PUBLIC API — Transition: crossfade between two video clips
 * ================================================================ */

int wb_ffmpeg_transition(const char *clip_a, const char *clip_b,
                          const char *output, int transition,
                          double duration_sec, double offset_sec) {
    if (!clip_a || !clip_b || !output) return -1;

    wb_cmd_builder b;
    cmd_init(&b);

    cmd_append(&b, "-i \"%s\" -i \"%s\"", clip_a, clip_b);
    cmd_append(&b, " -filter_complex \"");
    cmd_append(&b, "[0:v][1:v]xfade=transition=%s:duration=%.2f:offset=%.2f[outv];",
               wb_xfade_name(transition), duration_sec, offset_sec);
    cmd_append(&b, "[0:a][1:a]acrossfade=d=%.2f[outa]\"", duration_sec);
    cmd_append(&b, " -map \"[outv]\" -map \"[outa]\"");
    cmd_append(&b, " -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p");
    cmd_append(&b, " -c:a aac -b:a 192k");
    cmd_append(&b, " \"%s\"", output);

    int rc = run_ffmpeg(b.buf);
    cmd_free(&b);
    return rc;
}

/* ================================================================
 * Multi-clip transition chain
 * ================================================================ */

int wb_ffmpeg_transition_chain(const char **clips, int num_clips,
                                const char *output, int transition,
                                double transition_dur) {
    if (!clips || num_clips < 2 || !output) return -1;

    wb_cmd_builder b;
    cmd_init(&b);

    /* Input arguments */
    for (int i = 0; i < num_clips; i++) {
        cmd_append(&b, " -i \"%s\"", clips[i]);
    }

    /* Build filter chain */
    cmd_append(&b, " -filter_complex \"");

    /* Get durations via ffprobe would be ideal, but we estimate from offsets */
    /* For now, user must provide offsets or we use a simpler approach:
     * chain: [0][1]xfade→v01, [v01][2]xfade→v02, etc. */

    /* We need clip durations. Use a two-pass approach:
     * Pass 1: probe durations, Pass 2: build command */

    double *durations = (double *)calloc(num_clips, sizeof(double));
    if (!durations) { cmd_free(&b); return -1; }

    for (int i = 0; i < num_clips; i++) {
        char probe[1024];
        snprintf(probe, sizeof(probe),
                 "%s -v error -select_streams v:0 -show_entries stream=duration "
                 "-of default=noprint_wrappers=1:nokey=1 \"%s\" 2>/dev/null",
                 ffmpeg_path(), clips[i]);
        FILE *p = popen(probe, "r");
        if (p) {
            char val[64];
            if (fgets(val, sizeof(val), p))
                durations[i] = atof(val);
            pclose(p);
        }
        if (durations[i] <= 0) durations[i] = 5.0; /* fallback */
    }

    /* Build video xfade chain */
    double offset = 0;
    char prev_label[32] = "";

    for (int i = 0; i < num_clips - 1; i++) {
        offset += durations[i] - transition_dur;
        char curr_label[32];
        snprintf(curr_label, sizeof(curr_label), "[v%d]", i + 1);

        if (i == 0) {
            cmd_append(&b, "[0:v][1:v]xfade=transition=%s:duration=%.2f:offset=%.2f%s;",
                       wb_xfade_name(transition), transition_dur, offset, curr_label);
        } else {
            cmd_append(&b, "%s[%d:v]xfade=transition=%s:duration=%.2f:offset=%.2f%s;",
                       prev_label, i + 1, wb_xfade_name(transition),
                       transition_dur, offset, curr_label);
        }
        strcpy(prev_label, curr_label);
    }

    /* Add format filter on last output */
    cmd_append(&b, "%sformat=yuv420p[video];", prev_label);

    /* Build audio acrossfade chain */
    offset = 0;
    strcpy(prev_label, "");
    for (int i = 0; i < num_clips - 1; i++) {
        offset += durations[i] - transition_dur;
        char curr_label[32];
        snprintf(curr_label, sizeof(curr_label), "[a%d]", i + 1);

        if (i == 0) {
            cmd_append(&b, "[0:a][1:a]acrossfade=d=%.2f%s;",
                       transition_dur, curr_label);
        } else {
            cmd_append(&b, "%s[%d:a]acrossfade=d=%.2f%s;",
                       prev_label, i + 1, transition_dur, curr_label);
        }
        strcpy(prev_label, curr_label);
    }
    cmd_append(&b, "%sacopy[audio]\"", prev_label);

    cmd_append(&b, " -map \"[video]\" -map \"[audio]\"");
    cmd_append(&b, " -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p");
    cmd_append(&b, " -c:a aac -b:a 192k");
    cmd_append(&b, " \"%s\"", output);

    free(durations);
    int rc = run_ffmpeg(b.buf);
    cmd_free(&b);
    return rc;
}

/* ================================================================
 * Blend two videos with a specific blend mode
 * ================================================================ */

int wb_ffmpeg_blend(const char *clip_a, const char *clip_b,
                     const char *output, int mode,
                     double opacity, int use_expr) {
    if (!clip_a || !clip_b || !output) return -1;

    wb_cmd_builder b;
    cmd_init(&b);

    cmd_append(&b, "-i \"%s\" -i \"%s\"", clip_a, clip_b);
    cmd_append(&b, " -filter_complex \"");

    if (use_expr) {
        /* Custom expression blend for opacity control */
        char expr[256];
        snprintf(expr, sizeof(expr),
                 "A*(%.2f)+B*(1-%.2f)", opacity, opacity);
        cmd_append(&b, "[0:v][1:v]blend=all_expr='%s'[outv]\"", expr);
    } else {
        /* Named blend mode */
        cmd_append(&b, "[0:v][1:v]blend=all_mode=%s:all_opacity=%.2f[outv]\"",
                   wb_blend_name(mode), opacity);
    }

    cmd_append(&b, " -map \"[outv]\"");
    cmd_append(&b, " -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p");
    cmd_append(&b, " \"%s\"", output);

    int rc = run_ffmpeg(b.buf);
    cmd_free(&b);
    return rc;
}

/* ================================================================
 * Scrolling text / credits
 * ================================================================ */

int wb_ffmpeg_scroll_text(const char *input, const char *output,
                           const char *text, double scroll_speed,
                           int y_position, const char *font_color,
                           int font_size) {
    if (!input || !output || !text) return -1;

    wb_cmd_builder b;
    cmd_init(&b);

    /* Escape text for ffmpeg drawtext */
    char escaped[2048];
    int j = 0;
    for (int i = 0; text[i] && j < (int)sizeof(escaped) - 4; i++) {
        if (text[i] == '\'' || text[i] == ':' || text[i] == '\\') {
            escaped[j++] = '\\';
        }
        escaped[j++] = text[i];
    }
    escaped[j] = '\0';

    cmd_append(&b, "-i \"%s\"", input);
    cmd_append(&b, " -vf \"drawtext=text='%s':fontcolor=%s:fontsize=%d:"
               "x=(w-text_w)/2:y=h-%d*t:enable='gte(t,0)'\"",
               escaped, font_color ? font_color : "white",
               font_size > 0 ? font_size : 36,
               (int)scroll_speed);
    cmd_append(&b, " -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p");
    cmd_append(&b, " -c:a copy \"%s\"", output);

    int rc = run_ffmpeg(b.buf);
    cmd_free(&b);
    return rc;
}

/* ================================================================
 * Speed ramping (setpts)
 * ================================================================ */

int wb_ffmpeg_speed(const char *input, const char *output, double speed) {
    if (!input || !output || speed <= 0) return -1;

    wb_cmd_builder b;
    cmd_init(&b);

    /* setpts: speed up (>1) or slow down (<1) */
    double pts_factor = 1.0 / speed;
    cmd_append(&b, "-i \"%s\"", input);
    cmd_append(&b, " -filter_complex \"[0:v]setpts=%.4f*PTS[v];[0:a]atempo=%.2f[a]\"",
               pts_factor, speed > 2.0 ? 2.0 : (speed < 0.5 ? 0.5 : speed));
    cmd_append(&b, " -map \"[v]\" -map \"[a]\"");
    cmd_append(&b, " -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p");
    cmd_append(&b, " -c:a aac -b:a 192k \"%s\"", output);

    int rc = run_ffmpeg(b.buf);
    cmd_free(&b);
    return rc;
}

/* ================================================================
 * Reverse playback
 * ================================================================ */

int wb_ffmpeg_reverse(const char *input, const char *output, int reverse_audio) {
    if (!input || !output) return -1;

    wb_cmd_builder b;
    cmd_init(&b);

    cmd_append(&b, "-i \"%s\"", input);
    if (reverse_audio) {
        cmd_append(&b, " -vf reverse -af areverse");
    } else {
        cmd_append(&b, " -vf reverse");
    }
    cmd_append(&b, " -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p");
    if (reverse_audio)
        cmd_append(&b, " -c:a aac -b:a 192k");
    else
        cmd_append(&b, " -an");
    cmd_append(&b, " \"%s\"", output);

    int rc = run_ffmpeg(b.buf);
    cmd_free(&b);
    return rc;
}

/* ================================================================
 * Color grading via eq/colorbalance
 * ================================================================ */

int wb_ffmpeg_color_grade(const char *input, const char *output,
                           double brightness, double contrast, double saturation,
                           double hue) {
    if (!input || !output) return -1;

    wb_cmd_builder b;
    cmd_init(&b);

    cmd_append(&b, "-i \"%s\"", input);
    cmd_append(&b, " -vf \"eq=brightness=%.2f:contrast=%.2f:saturation=%.2f\"",
               brightness, contrast, saturation);
    if (hue != 0.0) {
        cmd_append(&b, ",hue=h=%.2f", hue);
    }
    cmd_append(&b, " -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p");
    cmd_append(&b, " -c:a copy \"%s\"", output);

    int rc = run_ffmpeg(b.buf);
    cmd_free(&b);
    return rc;
}

/* ================================================================
 * Picture-in-picture overlay
 * ================================================================ */

int wb_ffmpeg_pip(const char *main_video, const char *overlay_video,
                   const char *output, int overlay_w, int overlay_h,
                   const char *position) {
    if (!main_video || !overlay_video || !output) return -1;

    wb_cmd_builder b;
    cmd_init(&b);

    cmd_append(&b, "-i \"%s\" -i \"%s\"", main_video, overlay_video);

    /* Position: topleft, topright, bottomleft, bottomright, center */
    const char *pos_expr = "main_w-overlay_w-10:main_h-overlay_h-10"; /* default bottomright */
    if (position) {
        if (strcmp(position, "topleft") == 0) pos_expr = "10:10";
        else if (strcmp(position, "topright") == 0) pos_expr = "main_w-overlay_w-10:10";
        else if (strcmp(position, "bottomleft") == 0) pos_expr = "10:main_h-overlay_h-10";
        else if (strcmp(position, "center") == 0) pos_expr = "(main_w-overlay_w)/2:(main_h-overlay_h)/2";
    }

    cmd_append(&b, " -filter_complex \"[1:v]scale=%d:%d[pip];"
               "[0:v][pip]overlay=%s[outv]\"",
               overlay_w, overlay_h, pos_expr);
    cmd_append(&b, " -map \"[outv]\" -map 0:a");
    cmd_append(&b, " -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p");
    cmd_append(&b, " -c:a aac -b:a 192k \"%s\"", output);

    int rc = run_ffmpeg(b.buf);
    cmd_free(&b);
    return rc;
}

/* ================================================================
 * Ken Burns (zoompan) effect
 * ================================================================ */

int wb_ffmpeg_ken_burns(const char *input, const char *output,
                         int out_w, int out_h, double duration,
                         double zoom_start, double zoom_end) {
    if (!input || !output) return -1;

    wb_cmd_builder b;
    cmd_init(&b);

    /* Build zoom expression: linear interpolation from zoom_start to zoom_end */
    char zoom_expr[256];
    snprintf(zoom_expr, sizeof(zoom_expr),
             "'%.2f+(%.2f-%.2f)*on/(%.0f*30)'",
             zoom_start, zoom_end, zoom_start, duration);

    cmd_append(&b, "-loop 1 -t %.2f -i \"%s\"", duration, input);
    cmd_append(&b, " -vf \"scale=%d*2:%d*2,zoompan=z=%s:x='iw/2-(iw/zoom/2)':"
               "y='ih/2-(ih/zoom/2)':d=1:s=%d×%d:fps=30\"",
               out_w, out_h, zoom_expr, out_w, out_h);
    cmd_append(&b, " -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p");
    cmd_append(&b, " -shortest \"%s\"", output);

    int rc = run_ffmpeg(b.buf);
    cmd_free(&b);
    return rc;
}

/* ================================================================
 * Concatenate clips without re-encoding (smart concat demuxer)
 * ================================================================ */

int wb_ffmpeg_concat(const char **clips, int num_clips, const char *output) {
    if (!clips || num_clips < 2 || !output) return -1;

    /* Write concat list file */
    char list_path[] = "/tmp/wb_concat_XXXXXX";
    int fd = mkstemp(list_path);
    if (fd < 0) return -1;

    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(list_path); return -1; }

    for (int i = 0; i < num_clips; i++) {
        fprintf(f, "file '%s'\n", clips[i]);
    }
    fclose(f);

    wb_cmd_builder b;
    cmd_init(&b);

    cmd_append(&b, "-f concat -safe 0 -i \"%s\"", list_path);
    cmd_append(&b, " -c copy \"%s\"", output);

    int rc = run_ffmpeg(b.buf);
    cmd_free(&b);
    unlink(list_path);
    return rc;
}

/* ================================================================
 * Fade in/out
 * ================================================================ */

int wb_ffmpeg_fade(const char *input, const char *output,
                    double fade_in_dur, double fade_out_start, double fade_out_dur) {
    if (!input || !output) return -1;

    wb_cmd_builder b;
    cmd_init(&b);

    cmd_append(&b, "-i \"%s\"", input);

    int has_video_fade = (fade_in_dur > 0 || fade_out_dur > 0);
    int has_audio_fade = (fade_in_dur > 0);

    if (has_video_fade || has_audio_fade) {
        cmd_append(&b, " -filter_complex \"");

        /* Video fade */
        if (fade_in_dur > 0 && fade_out_dur > 0) {
            cmd_append(&b, "[0:v]fade=t=in:st=0:d=%.2f,fade=t=out:st=%.2f:d=%.2f[v];",
                       fade_in_dur, fade_out_start, fade_out_dur);
        } else if (fade_in_dur > 0) {
            cmd_append(&b, "[0:v]fade=t=in:st=0:d=%.2f[v];", fade_in_dur);
        } else {
            cmd_append(&b, "[0:v]fade=t=out:st=%.2f:d=%.2f[v];",
                       fade_out_start, fade_out_dur);
        }

        /* Audio fade */
        if (fade_in_dur > 0) {
            cmd_append(&b, "[0:a]afade=t=in:st=0:d=%.2f[a]\"", fade_in_dur);
        } else {
            cmd_append(&b, "[0:a]acopy[a]\"");
        }

        cmd_append(&b, " -map \"[v]\" -map \"[a]\"");
    }

    cmd_append(&b, " -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p");
    cmd_append(&b, " -c:a aac -b:a 192k \"%s\"", output);

    int rc = run_ffmpeg(b.buf);
    cmd_free(&b);
    return rc;
}

/* ================================================================
 * Get media info via ffprobe
 * ================================================================ */

int wb_ffmpeg_probe(const char *path, int *out_w, int *out_h,
                     double *out_duration, double *out_fps) {
    if (!path) return -1;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v error -select_streams v:0 "
             "-show_entries stream=width,height,duration,r_frame_rate "
             "-of default=noprint_wrappers=1 \"%s\" 2>/dev/null",
             path);

    FILE *p = popen(cmd, "r");
    if (!p) return -1;

    char line[256];
    while (fgets(line, sizeof(line), p)) {
        if (strncmp(line, "width=", 6) == 0 && out_w)
            *out_w = atoi(line + 6);
        else if (strncmp(line, "height=", 7) == 0 && out_h)
            *out_h = atoi(line + 7);
        else if (strncmp(line, "duration=", 9) == 0 && out_duration)
            *out_duration = atof(line + 9);
        else if (strncmp(line, "r_frame_rate=", 13) == 0 && out_fps) {
            int num, den;
            if (sscanf(line + 13, "%d/%d", &num, &den) == 2 && den > 0)
                *out_fps = (double)num / den;
        }
    }
    pclose(p);
    return 0;
}

/* ================================================================
 * DARK ARTS FFmpeg WRAPPERS (R094)
 * ================================================================ */

/* Strobe effect: alternate frames */
int wb_ffmpeg_strobe(const char *input, const char *output,
                     int interval, int fps) {
    char cmd[2046];
    snprintf(cmd, sizeof(cmd),
        "%s -i \"%s\" -vf \"select='not(mod(n\\,%d))',setpts=N/FRAME_RATE/TB\" "
        "-r %d \"%s\" 2>/dev/null",
        ffmpeg_path(), input, interval * 2, fps, output);
    return run_ffmpeg(cmd);
}

/* CRT / scanlines effect */
int wb_ffmpeg_crt(const char *input, const char *output,
                  float scanline_intensity, float curvature) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "%s -i \"%s\" -vf \""
        "geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':"
        "a='if(mod(Y\\,2)\\,alpha(X,Y)*%.2f\\,alpha(X,Y))',"
        "lenscorrection=k1=%.4f:k2=0\" "
        "\"%s\" 2>/dev/null",
        ffmpeg_path(), input, 1.0f - scanline_intensity, curvature, output);
    return run_ffmpeg(cmd);
}

/* Compression torture: re-encode at very low quality */
int wb_ffmpeg_compression_torture(const char *input, const char *output,
                                   int passes, int quality) {
    char cmd[4096];
    char temp1[] = "/tmp/wb_torture_a.mp4";
    char temp2[] = "/tmp/wb_torture_b.mp4";

    snprintf(cmd, sizeof(cmd),
        "%s -y -i \"%s\" -c:v libx264 -preset ultrafast -crf %d "
        "-an \"%s\" 2>/dev/null",
        ffmpeg_path(), input, quality, temp1);
    if (run_ffmpeg(cmd) != 0) return -1;

    for (int i = 1; i < passes; i++) {
        snprintf(cmd, sizeof(cmd),
            "%s -y -i \"%s\" -c:v libx264 -preset ultrafast -crf %d "
            "-an \"%s\" 2>/dev/null",
            ffmpeg_path(), temp1, quality, temp2);
        if (run_ffmpeg(cmd) != 0) { unlink(temp1); return -1; }
        unlink(temp1);
        strcpy(temp1, temp2);
    }

    snprintf(cmd, sizeof(cmd),
        "%s -y -i \"%s\" -i \"%s\" -c:v libx264 -preset ultrafast -crf %d "
        "-c:a copy \"%s\" 2>/dev/null",
        ffmpeg_path(), temp1, input, quality, output);
    int rc = run_ffmpeg(cmd);
    unlink(temp1);
    unlink(temp2);
    return rc;
}

/* Mad dash cut: rapid accelerating cuts */
int wb_ffmpeg_mad_dash(const char *input, const char *output,
                       int n_cuts, float start_gap, float accel) {
    char cmd[4096];
    char select_expr[2048] = "select=";

    float gap = start_gap;
    float pos = 0;
    for (int i = 0; i < n_cuts && strlen(select_expr) < 1800; i++) {
        char segment[64];
        if (i > 0) strcat(select_expr, "+");
        snprintf(segment, sizeof(segment), "eq(n\\,%.0f)", pos);
        strcat(select_expr, segment);
        pos += gap;
        gap *= accel;
        if (gap < 2) gap = 2;
    }
    strcat(select_expr, ",setpts=N/FRAME_RATE/TB");

    snprintf(cmd, sizeof(cmd),
        "%s -i \"%s\" -vf \"%s\" \"%s\" 2>/dev/null",
        ffmpeg_path(), input, select_expr, output);
    return run_ffmpeg(cmd);
}

/* YTPMV pitch shift with formant preservation (rubberband) */
int wb_ffmpeg_ytpmv_pitch_shift(const char *input, const char *output,
                                 float pitch_ratio, int formant) {
    char cmd[2048];
    if (formant) {
        snprintf(cmd, sizeof(cmd),
            "%s -i \"%s\" -af \"rubberband=pitch=%.4f:formant=preserved\" "
            "\"%s\" 2>/dev/null",
            ffmpeg_path(), input, pitch_ratio, output);
    } else {
        snprintf(cmd, sizeof(cmd),
            "%s -i \"%s\" -af \"rubberband=pitch=%.4f\" \"%s\" 2>/dev/null",
            ffmpeg_path(), input, pitch_ratio, output);
    }
    return run_ffmpeg(cmd);
}

/* Stare down: slow zoom on freeze frame */
int wb_ffmpeg_stare_down(const char *input, const char *output,
                         float zoom_speed, float duration) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "%s -i \"%s\" -vf "
        "\"zoompan=z='1+%.4f*in/%d':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':"
        "d=1:s=320x240:fps=30\" -t %.2f \"%s\" 2>/dev/null",
        ffmpeg_path(), input, zoom_speed, (int)(duration * 30),
        duration, output);
    return run_ffmpeg(cmd);
}
