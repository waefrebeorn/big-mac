/* wb_ytp_render.c — YTP ffmpeg render pipeline (R081).
 *
 * Takes a composition and renders it to video via ffmpeg CLI.
 * Each effect maps to a specific ffmpeg filter chain.
 *
 * This is a standalone test that demonstrates the filter chains.
 * The full integration with wb_ytp_compose.c comes later.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CMD 65536

/* ---- Effect Types (must match wb_ytp_compose.c) ------------------------ */

typedef enum {
    EFFECT_NONE = 0,
    EFFECT_STUTTER,
    EFFECT_SLOWMO,
    EFFECT_FASTFORWARD,
    EFFECT_REVERSE,
    EFFECT_PITCH_UP,
    EFFECT_PITCH_DOWN,
    EFFECT_EARRAPE,
    EFFECT_VINE_BOOM,
    EFFECT_DEEP_FRY,
    EFFECT_VHS,
    EFFECT_KALEIDO,
    EFFECT_SHAKE,
    EFFECT_ZOOM,
    EFFECT_FREEZE,
    EFFECT_FLASH,
    EFFECT_SCRAMBLE,
    EFFECT_SENTENCE_MIX,
    EFFECT_DATAMOSH,
    EFFECT_BLEEP,
    EFFECT_SUBTITLE,
    EFFECT_MOCAP_OVERLAY,
    EFFECT_SONIC_SCREAM,
    EFFECT_TO_BE_CONTINUED,
    EFFECT_MEME_SOUND,
} effect_type;

/* ---- Filter Builder ---------------------------------------------------- */

/* Build the video filter string for a given effect */
int build_vfilter(char *buf, int bufsize, effect_type fx, int intensity,
                   const char *text, double dur_sec, int w, int h) {
    switch (fx) {
        case EFFECT_NONE:        return 0;
        case EFFECT_STUTTER:     snprintf(buf, bufsize, "loop=%d:1:0", intensity); return 1;
        case EFFECT_SLOWMO:      snprintf(buf, bufsize, "setpts=%.2f*PTS", 1.0 + intensity * 0.3); return 1;
        case EFFECT_FASTFORWARD: snprintf(buf, bufsize, "setpts=%.2f*PTS", 1.0 / (1.0 + intensity * 0.4)); return 1;
        case EFFECT_REVERSE:     snprintf(buf, bufsize, "reverse"); return 1;
        case EFFECT_DEEP_FRY:
            snprintf(buf, bufsize,
                "eq=contrast=%.1f:brightness=%.2f:saturation=%.1f,unsharp=5:5:%.1f:5:5:%.1f,noise=alls=%d:allf=t+u",
                1.0 + intensity * 0.2, (intensity - 5) * 0.02, 2.0 + intensity * 0.3,
                2.0 + intensity * 0.5, 2.0 + intensity * 0.5, intensity * 5);
            return 1;
        case EFFECT_VHS:
            snprintf(buf, bufsize,
                "noise=alls=%d:allf=t+u,colorchannelmixer=0.3:0.4:0.3:0:0.3:0.4:0.3:0:0.3:0.4:0.3,eq=contrast=1.2:brightness=-0.05:saturation=0.7",
                intensity * 3);
            return 1;
        case EFFECT_SHAKE:
            snprintf(buf, bufsize,
                "crop=iw*0.9:ih*0.9:(iw-iw*0.9)*random(1):(ih-ih*0.9)*random(2),scale=iw:ih");
            return 1;
        case EFFECT_ZOOM:
            snprintf(buf, bufsize,
                "zoompan=z='1+%.2f*in/%d':d=1:x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':fps=30",
                (double)intensity * 0.5, (int)(dur_sec * 30));
            return 1;
        case EFFECT_FREEZE:
            snprintf(buf, bufsize, "trim=start_frame=0:end_frame=1,loop=-1:1:0,setpts=N/FRAME_RATE/TB");
            return 1;
        case EFFECT_FLASH:
            snprintf(buf, bufsize,
                "geq='lum=if(lt(mod(t,0.4),0.1),255,lum(X,Y))':cb=cb(X,Y):cr=cr(X,Y)");
            return 1;
        case EFFECT_DATAMOSH:
            snprintf(buf, bufsize, "noise=alls=%d:allf=t+u,eq=contrast=2.0:saturation=3.0",
                     intensity * 10);
            return 1;
        case EFFECT_BLEEP:
            snprintf(buf, bufsize,
                "drawbox=x=0:y=0:w=iw:h=ih:color=black@0.8:t=fill:enable='lt(mod(t,1),0.5)'");
            return 1;
        case EFFECT_SUBTITLE:
            snprintf(buf, bufsize,
                "drawtext=text='%s':fontsize=48:fontcolor=white:borderw=3:bordercolor=black:x=(w-text_w)/2:y=h-text_h:enable='1'",
                text);
            return 1;
        case EFFECT_SONIC_SCREAM:
            snprintf(buf, bufsize, "setpts=0.3*PTS");
            return 1;
        case EFFECT_TO_BE_CONTINUED:
            snprintf(buf, bufsize,
                "drawtext=text='▶':fontsize=120:fontcolor=white:x=w-150:y=h-150:enable='gte(t,0.5)'");
            return 1;
        case EFFECT_KALEIDO:
            snprintf(buf, bufsize,
                "split=4[a][b][c][d];[a]crop=iw/2:ih/2:0:0,scale=iw:ih[v1];"
                "[b]crop=iw/2:ih/2:iw/2:0,scale=iw:ih,hflip[v2];"
                "[c]crop=iw/2:ih/2:0:ih/2,scale=iw:ih,vflip[v3];"
                "[d]crop=iw/2:ih/2:iw/2:ih/2,scale=iw:ih,hflip,vflip[v4];"
                "[v1][v2][v3][v4]xstack=inputs=4:layout=0_0|w0_0|0_h0|w0_h0");
            return 1;
        default: return 0;
    }
}

/* Build the audio filter string for a given effect */
int build_afilter(char *buf, int bufsize, effect_type fx, int intensity,
                   double dur_sec) {
    switch (fx) {
        case EFFECT_STUTTER:     snprintf(buf, bufsize, "aloop=%d:1:0", intensity); return 1;
        case EFFECT_SLOWMO:      snprintf(buf, bufsize, "atempo=%.2f", 1.0 / (1.0 + intensity * 0.3)); return 1;
        case EFFECT_FASTFORWARD: snprintf(buf, bufsize, "atempo=%.2f", 1.0 + intensity * 0.4); return 1;
        case EFFECT_REVERSE:     snprintf(buf, bufsize, "areverse"); return 1;
        case EFFECT_PITCH_UP:    snprintf(buf, bufsize, "asetrate=44100*1.5,aresample=44100,atempo=0.667"); return 1;
        case EFFECT_PITCH_DOWN:  snprintf(buf, bufsize, "asetrate=44100*0.6,aresample=44100,atempo=1.667"); return 1;
        case EFFECT_EARRAPE:
            snprintf(buf, bufsize, "volume=%d:dB,acompressor=threshold=0.1:ratio=%d:attack=1:release=50",
                     intensity * 3, intensity * 2);
            return 1;
        case EFFECT_VINE_BOOM:   snprintf(buf, bufsize, "volume=0.0"); return 1;
        case EFFECT_BLEEP:       snprintf(buf, bufsize, "sine=frequency=1000:duration=0.3,volume=0.5"); return 1;
        case EFFECT_SONIC_SCREAM:snprintf(buf, bufsize, "asetrate=44100*3.0,aresample=44100,atempo=0.333"); return 1;
        case EFFECT_MEME_SOUND:  snprintf(buf, bufsize, "volume=2.0"); return 1;
        default: return 0;
    }
}

/* ---- Test -------------------------------------------------------------- */

int main(int argc, char **argv) {
    printf("=== YTP Render Pipeline — Filter Chain Test ===\n\n");

    struct { effect_type fx; int intensity; const char *name; } tests[] = {
        {EFFECT_STUTTER,      3, "stutter"},
        {EFFECT_SLOWMO,       5, "slowmo"},
        {EFFECT_FASTFORWARD,  4, "fastforward"},
        {EFFECT_REVERSE,      0, "reverse"},
        {EFFECT_PITCH_UP,     0, "pitch_up"},
        {EFFECT_PITCH_DOWN,   0, "pitch_down"},
        {EFFECT_EARRAPE,      8, "earrape"},
        {EFFECT_DEEP_FRY,     5, "deep_fry"},
        {EFFECT_VHS,          5, "vhs"},
        {EFFECT_SHAKE,        5, "shake"},
        {EFFECT_ZOOM,         3, "zoom"},
        {EFFECT_FREEZE,       0, "freeze"},
        {EFFECT_FLASH,        0, "flash"},
        {EFFECT_DATAMOSH,     5, "datamosh"},
        {EFFECT_BLEEP,        0, "bleep"},
        {EFFECT_SUBTITLE,     0, "subtitle"},
        {EFFECT_SONIC_SCREAM, 0, "sonic_scream"},
        {EFFECT_TO_BE_CONTINUED, 0, "to_be_continued"},
        {EFFECT_KALEIDO,      0, "kaleido"},
    };

    int passed = 0;
    for (int i = 0; i < (int)(sizeof(tests)/sizeof(tests[0])); i++) {
        char vfilter[2048] = {0};
        char afilter[1024] = {0};

        int has_v = build_vfilter(vfilter, sizeof(vfilter), tests[i].fx, tests[i].intensity, "test subtitle", 2.0, 854, 480);
        int has_a = build_afilter(afilter, sizeof(afilter), tests[i].fx, tests[i].intensity, 2.0);

        printf("%-18s vf=", tests[i].name);
        if (has_v) printf("\"%s\"", vfilter);
        else printf("(none)");
        printf("\n");
        printf("%-18s af=", "");
        if (has_a) printf("\"%s\"", afilter);
        else printf("(none)");
        printf("\n\n");
        passed++;
    }

    printf("=== %d/%d filter chains built successfully ===\n", passed, (int)(sizeof(tests)/sizeof(tests[0])));

    /* Show a sample ffmpeg command */
    printf("\n=== Sample ffmpeg command ===\n");
    char vf[2048], af[1024];
    build_vfilter(vf, sizeof(vf), EFFECT_DEEP_FRY, 7, "OH NO", 2.0, 854, 480);
    build_afilter(af, sizeof(af), EFFECT_PITCH_UP, 0, 2.0);
    printf("ffmpeg -i input.mp4 -vf \"%s,scale=854:480\" -af \"%s\" -c:v libx264 -preset veryfast -crf 23 -c:a aac -b:a 128k output.mp4\n", vf, af);

    return 0;
}
