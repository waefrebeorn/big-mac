/* wb_export_presets.h — export presets for streaming platforms.
 *
 * R077: Platform-specific encoding parameters for YouTube, TikTok, etc.
 *
 * Pure C11. Header-only.
 */

#ifndef WB_EXPORT_PRESETS_H
#define WB_EXPORT_PRESETS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    int         width;
    int         height;
    int         fps;
    int         video_bitrate_kbps;
    int         audio_bitrate_kbps;
    int         audio_sample_rate;
    const char *video_codec;
    const char *audio_codec;
    const char *container;
    int         pixel_format;  /* 0=yuv420p */
    int         profile;       /* 0=baseline, 1=main, 2=high */
    int         level;         /* 30=3.0, 40=4.0, 42=4.2, 51=5.1 */
    const char *extra_flags;
} wb_export_preset;

/* YouTube presets */
static const wb_export_preset EXPORT_YOUTUBE_1080P = {
    .name = "YouTube 1080p",
    .width = 1920, .height = 1080, .fps = 30,
    .video_bitrate_kbps = 8000, .audio_bitrate_kbps = 320,
    .audio_sample_rate = 48000,
    .video_codec = "libx264", .audio_codec = "aac",
    .container = "mp4",
    .pixel_format = 0, .profile = 2, .level = 40,
    .extra_flags = "-preset slow -crf 18 -movflags +faststart"
};

static const wb_export_preset EXPORT_YOUTUBE_4K = {
    .name = "YouTube 4K",
    .width = 3840, .height = 2160, .fps = 30,
    .video_bitrate_kbps = 35000, .audio_bitrate_kbps = 384,
    .audio_sample_rate = 48000,
    .video_codec = "libx264", .audio_codec = "aac",
    .container = "mp4",
    .pixel_format = 0, .profile = 2, .level = 51,
    .extra_flags = "-preset slow -crf 16 -movflags +faststart"
};

/* TikTok / Shorts / Reels (vertical 9:16) */
static const wb_export_preset EXPORT_TIKTOK = {
    .name = "TikTok",
    .width = 1080, .height = 1920, .fps = 30,
    .video_bitrate_kbps = 8000, .audio_bitrate_kbps = 128,
    .audio_sample_rate = 48000,
    .video_codec = "libx264", .audio_codec = "aac",
    .container = "mp4",
    .pixel_format = 0, .profile = 2, .level = 42,
    .extra_flags = "-preset fast -crf 20 -movflags +faststart"
};

static const wb_export_preset EXPORT_YOUTUBE_SHORTS = {
    .name = "YouTube Shorts",
    .width = 1080, .height = 1920, .fps = 30,
    .video_bitrate_kbps = 10000, .audio_bitrate_kbps = 256,
    .audio_sample_rate = 48000,
    .video_codec = "libx264", .audio_codec = "aac",
    .container = "mp4",
    .pixel_format = 0, .profile = 2, .level = 42,
    .extra_flags = "-preset fast -crf 18 -movflags +faststart"
};

static const wb_export_preset EXPORT_INSTAGRAM_REELS = {
    .name = "Instagram Reels",
    .width = 1080, .height = 1920, .fps = 30,
    .video_bitrate_kbps = 8000, .audio_bitrate_kbps = 128,
    .audio_sample_rate = 44100,
    .video_codec = "libx264", .audio_codec = "aac",
    .container = "mp4",
    .pixel_format = 0, .profile = 2, .level = 40,
    .extra_flags = "-preset fast -crf 20 -movflags +faststart -vf \"scale=1080:1920\""
};

/* Audio-only presets */
static const wb_export_preset EXPORT_SPOTIFY = {
    .name = "Spotify",
    .width = 0, .height = 0, .fps = 0,
    .video_bitrate_kbps = 0, .audio_bitrate_kbps = 320,
    .audio_sample_rate = 44100,
    .video_codec = NULL, .audio_codec = "libmp3lame",
    .container = "mp3",
    .pixel_format = 0, .profile = 0, .level = 0,
    .extra_flags = "-q:a 0"
};

static const wb_export_preset EXPORT_APPLE_MUSIC = {
    .name = "Apple Music",
    .width = 0, .height = 0, .fps = 0,
    .video_bitrate_kbps = 0, .audio_bitrate_kbps = 256,
    .audio_sample_rate = 44100,
    .video_codec = NULL, .audio_codec = "aac",
    .container = "m4a",
    .pixel_format = 0, .profile = 0, .level = 0,
    .extra_flags = "-b:a 256k"
};

/* Get preset by index (0-6). */
static inline const wb_export_preset* wb_export_get_preset(int index) {
    static const wb_export_preset *presets[] = {
        &EXPORT_YOUTUBE_1080P,
        &EXPORT_YOUTUBE_4K,
        &EXPORT_TIKTOK,
        &EXPORT_YOUTUBE_SHORTS,
        &EXPORT_INSTAGRAM_REELS,
        &EXPORT_SPOTIFY,
        &EXPORT_APPLE_MUSIC
    };
    if (index < 0 || index > 6) return presets[0];
    return presets[index];
}

/* Get preset by name. */
static inline const wb_export_preset* wb_export_find_preset(const char *name) {
    for (int i = 0; i < 7; i++) {
        const wb_export_preset *p = wb_export_get_preset(i);
        if (strstr(p->name, name) || strstr(name, p->name)) {
            return p;
        }
    }
    return &EXPORT_YOUTUBE_1080P;  /* default */
}

/* Build ffmpeg command string from preset. */
static inline int wb_export_build_cmd(const wb_export_preset *preset,
                                       const char *input,
                                       const char *output,
                                       char *cmd_buf, int buf_size) {
    if (preset->video_codec) {
        return snprintf(cmd_buf, buf_size,
            "ffmpeg -i \"%s\" -c:v %s -pix_fmt yuv420p -profile:v %s -level %d "
            "-b:v %dk -r %d -c:a %s -b:a %dk -ar %d %s \"%s\"",
            input,
            preset->video_codec,
            preset->profile == 0 ? "baseline" : (preset->profile == 1 ? "main" : "high"),
            preset->level,
            preset->video_bitrate_kbps,
            preset->fps,
            preset->audio_codec,
            preset->audio_bitrate_kbps,
            preset->audio_sample_rate,
            preset->extra_flags,
            output);
    } else {
        return snprintf(cmd_buf, buf_size,
            "ffmpeg -i \"%s\" -c:a %s -b:a %dk -ar %d %s \"%s\"",
            input,
            preset->audio_codec,
            preset->audio_bitrate_kbps,
            preset->audio_sample_rate,
            preset->extra_flags,
            output);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* WB_EXPORT_PRESETS_H */
