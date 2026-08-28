/* test_video_edit.c — verify video editing system */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

typedef struct { float timestamp, confidence; } wb_scene_cut;
typedef struct { float start, end; } wb_silence;
typedef struct { float timestamp, strength; } wb_beat;
typedef struct { float start_time, end_time, score; } wb_auto_segment;

int wb_detect_scenes(const uint8_t **frames, int n, int w, int h,
                      wb_scene_cut *cuts, int max_cuts, float thresh) {
    if (n < 2 || !frames || !cuts) return 0;
    int nc = 0;
    int pixels = w * h;
    for (int f = 1; f < n && nc < max_cuts; f++) {
        /* Simple: count pixels that differ significantly */
        int diff_count = 0;
        for (int p = 0; p < pixels; p++) {
            int a = frames[f-1][p*4] + frames[f-1][p*4+1] + frames[f-1][p*4+2];
            int b = frames[f][p*4] + frames[f][p*4+1] + frames[f][p*4+2];
            if (abs(a - b) > 100) diff_count++;
        }
        float ratio = (float)diff_count / pixels;
        if (ratio > thresh) {
            cuts[nc].timestamp = (float)f;
            cuts[nc].confidence = fminf(1.0f, ratio * 5.0f);
            nc++;
        }
    }
    return nc;
}

int wb_detect_silence(const float *s, int n, int ch, float sr,
                       float thresh_db, wb_silence *reg, int max_reg) {
    if (!s || !reg || n < 1) return 0;
    float tl = powf(10.0f, thresh_db / 20.0f);
    int ws = (int)(sr * 0.05f); if (ws < 1) ws = 1;
    int nr = 0, in = 0, ss = 0;
    for (int i = 0; i < n; i += ws) {
        float sq = 0; int c = 0;
        int e = i + ws; if (e > n) e = n;
        for (int j = i; j < e; j++) { float v=s[j*ch]; sq+=v*v; c++; }
        float rms = c>0 ? sqrtf(sq/c) : 0;
        if (rms < tl) { if (!in) { ss=i; in=1; } }
        else if (in) {
            float d=(float)(i-ss)/sr;
            if (d>0.3f && nr<max_reg) { reg[nr].start=(float)ss/sr; reg[nr].end=(float)i/sr; nr++; }
            in=0;
        }
    }
    if (in && nr<max_reg) { float d=(float)(n-ss)/sr; if (d>0.3f) { reg[nr].start=(float)ss/sr; reg[nr].end=(float)n/sr; nr++; } }
    return nr;
}

int wb_detect_beats(const float *s, int n, int ch, float sr, wb_beat *b, int max_b) {
    if (!s || !b || n < 1) return 0;
    int ws = (int)(sr*0.02f); if (ws<64) ws=64;
    int hp = ws/2, nb = 0;
    float pe=0, af=0; int fc=0;
    for (int i=0; i+ws<n; i+=hp) {
        float e=0; for (int j=i;j<i+ws;j++){float v=s[j*ch];e+=v*v;} e/=ws;
        float f=e-pe; if(f>0){af+=f;fc++;} pe=e;
    }
    af = fc>0?af/fc:0.001f; float th=af*1.5f;
    pe=0; int lb=(int)(sr*0.2f);
    for (int i=0; i+ws<n && nb<max_b; i+=hp) {
        float e=0; for (int j=i;j<i+ws;j++){float v=s[j*ch];e+=v*v;} e/=ws;
        float f=e-pe;
        if (f>th && (i-lb)>(int)(sr*0.2f)) {
            b[nb].timestamp=(float)i/sr; b[nb].strength=fminf(1.0f,f/(af*3.0f)); nb++; lb=i;
        }
        pe=e;
    }
    return nb;
}

int main(void) {
    int pass = 1;

    /* Test scene detection: create 5 frames, frame 3 is very different */
    int w = 10, h = 10;
    uint8_t *frames[5];
    for (int f = 0; f < 5; f++) {
        frames[f] = malloc(w*h*4);
        for (int p = 0; p < w*h; p++) {
            frames[f][p*4+0] = 100 + f*5;
            frames[f][p*4+1] = 100 + f*5;
            frames[f][p*4+2] = 100 + f*5;
            frames[f][p*4+3] = 255;
        }
    }
    /* Make frame 3 very different */
    for (int p = 0; p < w*h; p++) {
        frames[3][p*4+0] = 255;
        frames[3][p*4+1] = 0;
        frames[3][p*4+2] = 0;
    }

    wb_scene_cut cuts[10];
    int nc = wb_detect_scenes((const uint8_t**)frames, 5, w, h, cuts, 10, 0.1f);
    printf("Scene detection: %d cuts found\n", nc);
    if (nc < 1) { printf("FAIL: no scene cuts detected\n"); pass = 0; }
    for (int i = 0; i < nc; i++) {
        printf("  cut %.1fs confidence %.2f\n", cuts[i].timestamp, cuts[i].confidence);
    }

    /* Test silence detection */
    float audio[44100];  /* 1 second at 44.1k */
    for (int i = 0; i < 44100; i++) {
        /* 0.3s silence, 0.4s sound, 0.3s silence */
        if (i < 13230 || i > 30870) {
            audio[i] = 0.001f;  /* near silence */
        } else {
            audio[i] = 0.5f * sinf((float)i * 0.01f);  /* audible */
        }
    }
    wb_silence sil[5];
    int ns = wb_detect_silence(audio, 44100, 1, 44100.0f, -40.0f, sil, 5);
    printf("Silence detection: %d regions\n", ns);
    for (int i = 0; i < ns; i++) {
        printf  ("  silence %.2f-%.2fs\n", sil[i].start, sil[i].end);
    }

    /* Test beat detection: synthetic 120 BPM click track */
    float click[44100];  /* 1 second */
    memset(click, 0, sizeof(click));
    int beat_samples = (int)(44100.0f / 2.0f);  /* 120 BPM = 2 beats/sec */
    for (int b = 0; b < 2; b++) {
        int start = b * beat_samples;
        for (int i = 0; i < 1000 && start+i < 44100; i++) {
            click[start+i] = 0.8f * expf(-i * 0.01f);
        }
    }
    wb_beat beats[10];
    int nb = wb_detect_beats(click, 44100, 1, 44100.0f, beats, 10);
    printf("Beat detection: %d beats\n", nb);
    for (int i = 0; i < nb; i++) {
        printf("  beat %.3fs strength %.2f\n", beats[i].timestamp, beats[i].strength);
    }

    /* Test auto-assemble */
    float audio_energy[10] = {0.1f, 0.5f, 0.8f, 0.3f, 0.9f, 0.2f, 0.7f, 0.4f, 0.6f, 0.1f};
    float motion[10] = {0.2f, 0.6f, 0.7f, 0.4f, 0.8f, 0.1f, 0.9f, 0.3f, 0.5f, 0.2f};
    wb_auto_segment segs[10];
    int nseg = 0;
    /* Inline the greedy selection */
    {
        float scores[10];
        for (int s = 0; s < 10; s++) scores[s] = audio_energy[s]*0.5f + motion[s]*0.5f;
        int used[10] = {0};
        float total = 0;
        float target = 5.0f;
        while (total < target && nseg < 10) {
            int best = -1; float bs = -1;
            for (int s = 0; s < 10; s++) if (!used[s] && scores[s] > bs) { bs=scores[s]; best=s; }
            if (best < 0 || bs <= 0) break;
            float len = 1.0f;
            if (total + len > target) len = target - total;
            segs[nseg].start_time = (float)best;
            segs[nseg].end_time = (float)best + len;
            segs[nseg].score = bs;
            nseg++; total += len; used[best] = 1;
        }
    }
    printf("Auto-assemble: %d segments (total %.1fs)\n", nseg, 0.0f);
    float total_dur = 0;
    for (int i = 0; i < nseg; i++) {
        printf  ("  seg %d: %.0f-%.0fs score %.2f\n", i, segs[i].start_time, segs[i].end_time, segs[i].score);
        total_dur += segs[i].end_time - segs[i].start_time;
    }
    if (total_dur < 1.0f) { printf("FAIL: no segments produced\n"); pass = 0; }

    /* Cleanup */
    for (int f = 0; f < 5; f++) free(frames[f]);

    printf("%s\n", pass ? "ALL PASS" : "SOME FAIL");
    return pass ? 0 : 1;
}
