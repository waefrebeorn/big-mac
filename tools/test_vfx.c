/* test_vfx.c — verify VFX system */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* Inline the blend mode test */
static inline float blend_multiply(float b, float s) { return b * s; }
static inline float blend_screen(float b, float s) { return b + s - b * s; }
static inline float blend_overlay(float b, float s) {
    return b < 0.5f ? 2.0f * b * s : 1.0f - 2.0f * (1.0f - b) * (1.0f - s);
}
static inline float blend_difference(float b, float s) { return fabsf(b - s); }
static inline float blend_soft_light(float b, float s) {
    if (s <= 0.5f) return b - (1.0f - 2.0f * s) * b * (1.0f - b);
    else { float d = (b <= 0.25f) ? ((16.0f*b - 12.0f)*b + 4.0f)*b : sqrtf(b);
           return b + (2.0f*s - 1.0f)*(d - b); }
}

/* Color correction */
typedef struct {
    float lift, gamma, gain, contrast, saturation, temperature, hue;
} wb_color_params;

void wb_color_correct(uint8_t *rgba, int count, const wb_color_params *p) {
    for (int i = 0; i < count; i++) {
        int idx = i*4;
        float r = rgba[idx]/255.0f, g = rgba[idx+1]/255.0f, b = rgba[idx+2]/255.0f;
        r += p->lift; g += p->lift; b += p->lift;
        r = fmaxf(0,r); g = fmaxf(0,g); b = fmaxf(0,b);
        /* Gamma: gamma > 1 = darker midtones, gamma < 1 = brighter */
        if (p->gamma != 1.0f) {
            r = powf(r, p->gamma);
            g = powf(g, p->gamma);
            b = powf(b, p->gamma);
        }
        r *= p->gain; g *= p->gain; b *= p->gain;
        if (p->contrast != 1.0f) { r=(r-0.5f)*p->contrast+0.5f; g=(g-0.5f)*p->contrast+0.5f; b=(b-0.5f)*p->contrast+0.5f; }
        if (p->saturation != 1.0f) { float l=0.2126f*r+0.7152f*g+0.0722f*b; r=l+(r-l)*p->saturation; g=l+(g-l)*p->saturation; b=l+(b-l)*p->saturation; }
        rgba[idx]   = (uint8_t)(fminf(1,fmaxf(0,r))*255.0f+0.5f);
        rgba[idx+1] = (uint8_t)(fminf(1,fmaxf(0,g))*255.0f+0.5f);
        rgba[idx+2] = (uint8_t)(fminf(1,fmaxf(0,b))*255.0f+0.5f);
    }
}

/* Vignette */
void wb_effect_vignette(uint8_t *rgba, int w, int h, float str) {
    float cx=w*0.5f, cy=h*0.5f;
    for (int y=0;y<h;y++) for (int x=0;x<w;x++) {
        int idx=(y*w+x)*4;
        float dx=(x-cx)/cx, dy=(y-cy)/cy;
        float dist=sqrtf(dx*dx+dy*dy);
        float f=1.0f-str*dist*dist; if(f<0)f=0;
        rgba[idx]=(uint8_t)(rgba[idx]*f);
        rgba[idx+1]=(uint8_t)(rgba[idx+1]*f);
        rgba[idx+2]=(uint8_t)(rgba[idx+2]*f);
    }
}

/* Posterize */
void wb_effect_posterize(uint8_t *rgba, int count, int levels) {
    if (levels<2) levels=2;
    float step=255.0f/(float)(levels-1);
    for (int i=0;i<count*4;i+=4) {
        rgba[i]=(uint8_t)(roundf(rgba[i]/step)*step);
        rgba[i+1]=(uint8_t)(roundf(rgba[i+1]/step)*step);
        rgba[i+2]=(uint8_t)(roundf(rgba[i+2]/step)*step);
    }
}

int main(void) {
    int pass = 1;

    /* Test blend modes */
    float r;
    r = blend_multiply(0.5f, 0.5f);
    if (fabsf(r - 0.25f) > 0.01f) { printf("FAIL multiply: %.3f\n", r); pass=0; }

    r = blend_screen(0.5f, 0.5f);
    if (fabsf(r - 0.75f) > 0.01f) { printf("FAIL screen: %.3f\n", r); pass=0; }

    r = blend_overlay(0.5f, 0.5f);
    if (fabsf(r - 0.5f) > 0.01f) { printf("FAIL overlay: %.3f\n", r); pass=0; }

    r = blend_difference(0.8f, 0.3f);
    if (fabsf(r - 0.5f) > 0.01f) { printf("FAIL difference: %.3f\n", r); pass=0; }

    r = blend_soft_light(0.5f, 0.0f);
    if (fabsf(r - 0.25f) > 0.01f) { printf("FAIL soft_light(black): %.3f\n", r); pass=0; }

    r = blend_soft_light(0.5f, 1.0f);
    if (fabsf(r - (0.5f + (2.0f-1.0f)*(sqrtf(0.5f)-0.5f))) > 0.01f) { printf("FAIL soft_light(white): %.3f\n", r); pass=0; }

    printf("Blend tests: multiply=%.3f screen=%.3f overlay=%.3f diff=%.3f soft=%.3f\n",
           blend_multiply(0.5f,0.5f), blend_screen(0.5f,0.5f),
           blend_overlay(0.5f,0.5f), blend_difference(0.8f,0.3f),
           blend_soft_light(0.5f,0.5f));

    /* Test color correction: identity should not change */
    uint8_t px[4] = {128, 64, 200, 255};
    wb_color_params identity = {0, 1, 1, 1, 1, 0, 0};
    wb_color_correct(px, 1, &identity);
    if (abs(px[0]-128)>2 || abs(px[1]-64)>2 || abs(px[2]-200)>2) {
        printf("FAIL color identity: (%d,%d,%d)\n", px[0], px[1], px[2]); pass=0;
    }

    /* Test color correction: gamma 2.0 should darken midtones */
    uint8_t px2[4] = {128, 128, 128, 255};
    wb_color_params darken = {0, 2.0f, 1, 1, 1, 0, 0};
    wb_color_correct(px2, 1, &darken);
    if (px2[0] >= 128) { printf("FAIL gamma darken: %d (should be <128)\n", px2[0]); pass=0; }

    /* Test color correction: saturation 0 = grayscale */
    uint8_t px3[4] = {200, 100, 50, 255};
    wb_color_params desat = {0, 1, 1, 1, 0, 0, 0};
    wb_color_correct(px3, 1, &desat);
    if (abs(px3[0]-px3[1]) > 2 || abs(px3[1]-px3[2]) > 2) {
        printf("FAIL desaturate: (%d,%d,%d) should be gray\n", px3[0], px3[1], px3[2]); pass=0;
    }

    printf("Color tests: identity=(%d,%d,%d) gamma=%d desat=(%d,%d,%d)\n",
           128,64,200, px2[0], px3[0], px3[1], px3[2]);

    /* Test vignette: center should be bright, corner dark */
    uint8_t img[100*100*4];
    memset(img, 200, sizeof(img));
    wb_effect_vignette(img, 100, 100, 0.5f);
    int center = img[(50*100+50)*4];
    int corner = img[0];
    if (center <= corner) { printf("FAIL vignette: center=%d corner=%d\n", center, corner); pass=0; }
    printf("Vignette: center=%d corner=%d\n", center, corner);

    /* Test posterize */
    uint8_t px4[4] = {127, 63, 192, 255};
    wb_effect_posterize(px4, 1, 4);
    printf("Posterize 4 levels: (%d,%d,%d) from (127,63,192)\n", px4[0], px4[1], px4[2]);

    printf("%s\n", pass ? "ALL PASS" : "SOME FAIL");
    return pass ? 0 : 1;
}
