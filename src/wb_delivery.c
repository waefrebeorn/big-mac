/* wb_delivery.c — delivery presets: loudnorm + chapters (R064). */

#include "wbus/wbus_delivery.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

/* G41 (Wave2): stems export — render each non-bus track to its own WAV.
 * Every stem starts at zero and spans the full session length, so the
 * files drop straight onto any DAW timeline in sync. */
int wb_delivery_export_stems(wb_session *s, const char *dir);

static void sanitize_name(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++)
        dst[o++] = (src[i] >= 'a' && src[i] <= 'z') ||
                   (src[i] >= 'A' && src[i] <= 'Z') ||
                   (src[i] >= '0' && src[i] <= '9') ? src[i] : '_';
    dst[o] = 0;
    if (!dst[0]) snprintf(dst, cap, "track");
}

int wb_delivery_export_stems(wb_session *s, const char *dir) {
    if (!s || !dir || s->length <= 0) return -1;
    mkdir(dir, 0755);
    int written = 0;
    for (uint32_t ti = 0; ti < s->track_count; ti++) {
        if (s->tracks[ti].kind == 2) continue;      /* mix bus: no stem */
        wb_session *one = wb_session_copy(s);       /* deep independent copy */
        if (!one) return written > 0 ? written : -1;
        /* solo this track by muting everything else */
        for (uint32_t m = 0; m < one->track_count; m++)
            one->tracks[m].mute = (m == ti) ? 0 : 1;
        wb_sample *pcm = NULL;
        uint32_t frames = 0;
        if (wb_engine_render_session(NULL, one, &pcm, &frames) == 0 && pcm) {
            char base[96], path[512];
            sanitize_name(base, sizeof base, s->tracks[ti].name);
            snprintf(path, sizeof path, "%s/track%02u_%s.wav", dir,
                     (unsigned)(written + 1), base);
            if (wb_wav_write_pcm16(path, pcm, frames, 2, WB_SAMPLE_RATE) == 0)
                written++;
            free(pcm);
        }
        wb_session_destroy(one);
    }
    return written;
}


#ifndef WB_DELIVERY_FFMPEG
#define WB_DELIVERY_FFMPEG "/Users/waefrebeorn/.local/bin/ffmpeg"
#endif

int wb_delivery_measure_loudness(const char *wav_path,
                                 double *i_out, double *tp_out,
                                 double *lra_out, double *thresh_out) {
    if (!wav_path || !i_out) return -1;
    char cmd[1200];
    snprintf(cmd, sizeof cmd,
        "\"%s\" -y -i \"%s\" -af loudnorm=print_format=json "
        "-f null /dev/null 2>&1",
        WB_DELIVERY_FFMPEG, wav_path);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;

    char line[512];
    int in_json = 0;
    double vals[4] = {0,0,0,0};
    int got = 0;
    while (fgets(line, sizeof line, f)) {
        /* find the JSON block keys */
        char *k;
        if ((k = strstr(line, "\"input_i\""))) {
            sscanf(k, "%*[^-0-9]%lf", &vals[0]); got |= 1;
        } else if ((k = strstr(line, "\"input_tp\""))) {
            sscanf(k, "%*[^-0-9]%lf", &vals[1]); got |= 2;
        } else if ((k = strstr(line, "\"input_lra\""))) {
            sscanf(k, "%*[^-0-9]%lf", &vals[2]); got |= 4;
        } else if ((k = strstr(line, "\"input_thresh\""))) {
            sscanf(k, "%*[^-0-9]%lf", &vals[3]); got |= 8;
        }
        (void)in_json;
    }
    pclose(f);
    if (got != 15) return -1;
    if (i_out)     *i_out     = vals[0];
    if (tp_out)    *tp_out    = vals[1];
    if (lra_out)   *lra_out   = vals[2];
    if (thresh_out)*thresh_out= vals[3];
    return 0;
}

/* G55: normalize honoring a named profile's LUFS target + TP ceiling. */
int wb_delivery_normalize_wav_profile(const char *wav_path,
                                      const wb_delivery_profile *prof);

int wb_delivery_normalize_wav(const char *wav_path, double target_lufs) {
    if (!wav_path || target_lufs > 0) return -1;
    double I, TP, LRA, TH;
    if (wb_delivery_measure_loudness(wav_path, &I,&TP,&LRA,&TH) != 0)
        return -1;
    char cmd[1600];
    const char *tmp = "/tmp/bigmac_norm.wav";
    snprintf(cmd, sizeof cmd,
        "\"%s\" -y -i \"%s\" -af "
        "loudnorm=linear=true:i=%.1f:lra=11.0:tp=-1.5:"
        "measured_I=%.2f:measured_TP=%.2f:measured_LRA=%.2f:"
        "measured_thresh=%.2f "
        "-ar 48000 \"%s\" >/dev/null 2>&1",
        WB_DELIVERY_FFMPEG, wav_path, target_lufs, I, TP, LRA, TH, tmp);
    int rc = system(cmd);
    if (rc != 0) return -1;
    /* swap back onto the original path */
    char rn[1400];
    snprintf(rn, sizeof rn, "mv %s %s", tmp, wav_path);
    return system(rn) == 0 ? 0 : -1;
}

/* ---- G55: named loudness profiles -------------------------------------- */
static const wb_delivery_profile g_profiles[] = {
    { "EBU-R128",  -23.0, -1.0, 0.0 },   /* broadcast (Europe) */
    { "ATSC-A85",  -24.0, -2.0, 0.0 },   /* US CALM Act */
    { "NETFLIX",   -27.0, -2.0, 18.0 },  /* dialogue-gated; LRA 4-18 */
    { "YOUTUBE",   -14.0, -1.5, 0.0 },   /* streaming */
    { "PODCAST",   -16.0, -1.5, 0.0 },   /* stereo podcast */
};

const wb_delivery_profile *wb_delivery_profiles(int *count_out) {
    if (count_out) *count_out = (int)(sizeof g_profiles / sizeof g_profiles[0]);
    return g_profiles;
}

const wb_delivery_profile *wb_delivery_profile_by_name(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_profiles / sizeof g_profiles[0]; i++)
        if (strcmp(g_profiles[i].name, name) == 0) return &g_profiles[i];
    return NULL;
}

int wb_delivery_normalize_wav_profile(const char *wav_path,
                                      const wb_delivery_profile *prof) {
    if (!wav_path || !prof || prof->lufs > 0 || prof->tp_ceiling >= 0)
        return -1;
    double I, TP, LRA, TH;
    if (wb_delivery_measure_loudness(wav_path, &I,&TP,&LRA,&TH) != 0)
        return -1;
    double lra_cap = prof->lra_max > 0 ? prof->lra_max : 11.0;
    char cmd[1600];
    const char *tmp = "/tmp/bigmac_norm.wav";
    snprintf(cmd, sizeof cmd,
        "\"%s\" -y -i \"%s\" -af "
        "loudnorm=linear=true:i=%.1f:lra=%.1f:tp=%.1f:"
        "measured_I=%.2f:measured_TP=%.2f:measured_LRA=%.2f:"
        "measured_thresh=%.2f "
        "-ar 48000 \"%s\" >/dev/null 2>&1",
        WB_DELIVERY_FFMPEG, wav_path, prof->lufs, lra_cap, prof->tp_ceiling,
        I, TP, LRA, TH, tmp);
    if (system(cmd) != 0) return -1;
    char rn[1400];
    snprintf(rn, sizeof rn, "mv %s %s", tmp, wav_path);
    return system(rn) == 0 ? 0 : -1;
}

int wb_delivery_chapters(const wb_session *s, char *buf, size_t cap) {
    if (!s || !buf || cap == 0) return 0;
    buf[0] = 0;
    if (s->marker_count < 2) return 0;      /* need >= 2 */
    /* YouTube requires first chapter at 0:00 */
    size_t off = 0;
    int written = 0;
    for (uint32_t i = 0; i < s->marker_count && off < cap; i++) {
        const wb_marker *mk = &s->markers[i];
        long total_sec = (long)(mk->pos / WB_SAMPLE_RATE + 0.5);
        long h = total_sec / 3600, m = (total_sec/60)%60, sec = total_sec%60;
        int n = snprintf(buf+off, cap-off, "%02ld:%02ld:%02ld %s\n",
                         h, m, sec, mk->label[0]?mk->label:"Chapter");
        if (n < 0 || (size_t)n >= cap-off) break;
        off += (size_t)n;
        written++;
    }
    return written;
}
