/* wb_delivery.c — delivery presets: loudnorm + chapters (R064). */

#include "wbus/wbus_delivery.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
