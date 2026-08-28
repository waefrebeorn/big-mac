/* test_ytp.c — verify YTP/meme editing engine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* Forward declarations */
int wb_stutter_loop(const float *in, float *out, int seg, int ch, int rep);
int wb_pitch_shift(const float *in, float *out, int n, int ch, float ratio);
void wb_earrape(float *s, int n, float m);
void wb_reverse(float *s, int n, int ch);
void wb_datamosh(uint8_t *o, const uint8_t *a, const uint8_t *b, int w, int h, float intensity);
void wb_generate_vine_boom(float *out, int n, int ch, float sr);
void wb_word_salad(int *w, int n);

int main(void) {
    int pass = 1;

    /* Test stutter loop */
    float seg[10] = {1,2,3,4,5,6,7,8,9,10};
    float out[50];
    int n = wb_stutter_loop(seg, out, 5, 1, 3);
    if (n != 15) { printf("FAIL stutter: %d frames\n", n); pass = 0; }
    /* Check: out[0..4] = seg, out[5..9] = seg, out[10..14] = seg */
    for (int i = 0; i < 5; i++) {
        if (out[i] != seg[i] || out[i+5] != seg[i] || out[i+10] != seg[i]) {
            printf("FAIL stutter content at %d: %.0f %.0f %.0f\n", i, out[i], out[i+5], out[i+10]);
            pass = 0; break;
        }
    }
    printf("Stutter: %d frames, content OK\n", n);

    /* Test pitch shift: ratio 2.0 should halve frame count */
    float audio[100];
    for (int i = 0; i < 100; i++) audio[i] = sinf(i * 0.1f);
    float shifted[100];
    int ns = wb_pitch_shift(audio, shifted, 100, 1, 2.0f);
    if (ns != 50) { printf("FAIL pitch shift: %d frames (expected 50)\n", ns); pass = 0; }
    printf("Pitch shift: 100 → %d frames (ratio 2.0)\n", ns);

    /* Test ear-rape */
    float ear[4] = {0.1f, 0.2f, -0.3f, 0.4f};
    wb_earrape(ear, 4, 5.0f);
    if (fabsf(ear[0] - 0.5f) > 0.01f) { printf("FAIL ear-rape: %.2f\n", ear[0]); pass = 0; }
    printf("Ear-rape: %.1f → %.1f (×5)\n", 0.1f, ear[0]);

    /* Test reverse */
    float rev[6] = {1,2,3,4,5,6};
    wb_reverse(rev, 6, 1);
    if (rev[0] != 6 || rev[5] != 1) { printf("FAIL reverse: %.0f..%.0f\n", rev[0], rev[5]); pass = 0; }
    printf("Reverse: 1,2,3,4,5,6 → %.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n",
           rev[0], rev[1], rev[2], rev[3], rev[4], rev[5]);

    /* Test datamosh: verify function runs without crash on edge case */
    uint8_t fa[16*16*4], fb[16*16*4], mo[16*16*4];
    for (int i = 0; i < 16*16*4; i += 4) {
        fa[i] = 100; fa[i+1] = 100; fa[i+2] = 100; fa[i+3] = 255;
        fb[i] = (i % 8 == 0) ? 200 : 100;
        fb[i+1] = (i % 8 == 0) ? 50 : 100;
        fb[i+2] = 100; fb[i+3] = 255;
    }
    wb_datamosh(mo, fa, fb, 16, 16, 1.0f);
    /* Just verify it ran and produced valid output */
    int valid = 1;
    for (int i = 0; i < 16*16*4; i += 4) {
        if (mo[i+3] != 255) { valid = 0; break; }  /* alpha preserved */
    }
    if (!valid) { printf("FAIL datamosh: invalid output\n"); pass = 0; }
    printf("Datamosh: ran successfully, alpha preserved\n");

    /* Test vine boom generation */
    float boom[4410];
    wb_generate_vine_boom(boom, 4410, 1, 44100.0f);
    float peak = 0;
    for (int i = 0; i < 4410; i++) {
        if (fabsf(boom[i]) > peak) peak = fabsf(boom[i]);
    }
    if (peak < 0.1f) { printf("FAIL vine boom: peak %.3f\n", peak); pass = 0; }
    printf("Vine boom: peak %.3f\n", peak);

    /* Test word salad (shuffle) */
    int words[10] = {0,1,2,3,4,5,6,7,8,9};
    wb_word_salad(words, 10);
    int sum = 0;
    for (int i = 0; i < 10; i++) sum += words[i];
    if (sum != 45) { printf("FAIL word salad: sum %d (elements lost)\n", sum); pass = 0; }
    printf("Word salad: shuffled, sum=%d (all elements preserved)\n", sum);

    printf("%s\n", pass ? "ALL PASS" : "SOME FAIL");
    return pass ? 0 : 1;
}

/* Inline implementations for standalone test */
int wb_stutter_loop(const float *in, float *out, int seg, int ch, int rep) {
    int ss = seg * ch;
    for (int r = 0; r < rep; r++) memcpy(out + r*ss, in, ss*sizeof(float));
    return rep * seg;
}

int wb_pitch_shift(const float *in, float *out, int n, int ch, float ratio) {
    int of = (int)(n / ratio);
    for (int i = 0; i < of; i++) {
        float sp = (float)i * ratio;
        int si = (int)sp;
        float f = sp - si;
        if (si+1 >= n) { for(int c=0;c<ch;c++) out[i*ch+c]=in[(n-1)*ch+c]; }
        else { for(int c=0;c<ch;c++) out[i*ch+c]=in[si*ch+c]*(1-f)+in[(si+1)*ch+c]*f; }
    }
    return of;
}

void wb_earrape(float *s, int n, float m) {
    for (int i=0;i<n;i++){s[i]*=m; if(s[i]>1)s[i]=1; if(s[i]<-1)s[i]=-1;}
}

void wb_reverse(float *s, int n, int ch) {
    for (int i=0;i<n/2;i++) { int o=n-1-i; for(int c=0;c<ch;c++){float t=s[i*ch+c];s[i*ch+c]=s[o*ch+c];s[o*ch+c]=t;} }
}

void wb_datamosh(uint8_t *o, const uint8_t *a, const uint8_t *b, int w, int h, float intensity) {
    memcpy(o, a, w*h*4);
    for (int y=1;y<h-1;y++) for (int x=1;x<w-1;x++) {
        int idx=(y*w+x)*4;
        int dr=b[idx]-a[idx], dg=b[idx+1]-a[idx+1];
        float mot=sqrtf((float)(dr*dr+dg*dg))/255.0f;
        if (mot>0.1f) {
            int ox=(int)(dr*intensity*0.1f), oy=(int)(dg*intensity*0.1f);
            int sx=x+ox, sy=y+oy;
            if (sx>=0&&sx<w&&sy>=0&&sy<h) { int si=(sy*w+sx)*4; o[idx]=a[si]; o[idx+1]=a[si+1]; o[idx+2]=a[si+2]; }
        }
    }
}

void wb_generate_vine_boom(float *out, int n, int ch, float sr) {
    int tl = (int)(sr*0.1f);
    for (int i=0;i<n;i++) {
        float s=0;
        if (i<tl) { float t=(float)i/sr; float e=expf(-t*30.0f); s=sinf(t*2.0f*3.14159f*80.0f)*e*0.8f;
            if (i<tl/4) { float noise=((float)rand()/RAND_MAX-0.5f)*2.0f; s+=noise*expf(-t*60.0f)*0.5f; } }
        for (int c=0;c<ch;c++) out[i*ch+c]=s;
    }
}

void wb_word_salad(int *w, int n) {
    for (int i=n-1;i>0;i--) { int j=rand()%(i+1); int t=w[i];w[i]=w[j];w[j]=t; }
}
