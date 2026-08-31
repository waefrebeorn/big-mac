/* wb_color_grading.c — advanced color grading (DaVinci Resolve style).
 *
 * Lift/Gamma/Gain color wheels, saturation, contrast, temperature, tint,
 * and 3D LUT processing. Pure C11. */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "wbus.h"

#define MAX_LUT_SIZE 64

typedef struct {
    /* Lift/Gamma/Gain: per-channel multipliers */
    float lift[3], gamma[3], gain[3];
    float saturation;
    float contrast;
    float temperature;
    float tint;

    /* 3D LUT */
    int lut_size;
    float lut[3][MAX_LUT_SIZE][MAX_LUT_SIZE][MAX_LUT_SIZE]; /* RGB */
    int lut_loaded;
} wb_color_grading;

void *wb_color_grading_create(int width, int height) {
    (void)width; (void)height;
    wb_color_grading *cg = (wb_color_grading *)calloc(1, sizeof(*cg));
    if (!cg) return NULL;
    cg->lift[0] = cg->lift[1] = cg->lift[2] = 1.0f;
    cg->gamma[0] = cg->gamma[1] = cg->gamma[2] = 1.0f;
    cg->gain[0] = cg->gain[1] = cg->gain[2] = 1.0f;
    cg->saturation = 1.0f;
    cg->contrast = 1.0f;
    cg->temperature = 0.0f;
    cg->tint = 0.0f;
    cg->lut_loaded = 0;
    cg->lut_size = 0;
    return cg;
}

void wb_color_grading_destroy(void *inst) { free(inst); }

void wb_color_grading_set_lift(void *inst, float r, float g, float b) {
    wb_color_grading *cg = (wb_color_grading *)inst;
    if (!cg) return;
    cg->lift[0] = r; cg->lift[1] = g; cg->lift[2] = b;
}

void wb_color_grading_set_gamma(void *inst, float r, float g, float b) {
    wb_color_grading *cg = (wb_color_grading *)inst;
    if (!cg) return;
    cg->gamma[0] = r; cg->gamma[1] = g; cg->gamma[2] = b;
}

void wb_color_grading_set_gain(void *inst, float r, float g, float b) {
    wb_color_grading *cg = (wb_color_grading *)inst;
    if (!cg) return;
    cg->gain[0] = r; cg->gain[1] = g; cg->gain[2] = b;
}

void wb_color_grading_set_saturation(void *inst, float sat) {
    wb_color_grading *cg = (wb_color_grading *)inst;
    if (!cg) return;
    cg->saturation = sat;
}

void wb_color_grading_set_contrast(void *inst, float contrast) {
    wb_color_grading *cg = (wb_color_grading *)inst;
    if (!cg) return;
    cg->contrast = contrast;
}

void wb_color_grading_set_temperature(void *inst, float temp) {
    wb_color_grading *cg = (wb_color_grading *)inst;
    if (!cg) return;
    cg->temperature = temp;
}

void wb_color_grading_set_tint(void *inst, float tint) {
    wb_color_grading *cg = (wb_color_grading *)inst;
    if (!cg) return;
    cg->tint = tint;
}

int wb_color_grading_load_lut(void *inst, const char *path) {
    wb_color_grading *cg = (wb_color_grading *)inst;
    if (!cg || !path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    int size = 0;
    float min_v = 0, max_v = 1;

    /* Parse .cube header */
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "LUT_3D_SIZE %d", &size) == 1) break;
        if (sscanf(line, "DOMAIN_MIN %f %f %f", &min_v, &min_v, &min_v) == 3) break;
    }
    if (size <= 0 || size > MAX_LUT_SIZE) size = 33; /* default */

    /* Read LUT data */
    float r, g, b;
    int idx = 0;
    int total = size * size * size;
    while (idx < total && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%f %f %f", &r, &g, &b) == 3) {
            int ri = idx % size;
            int gi = (idx / size) % size;
            int bi = idx / (size * size);
            cg->lut[0][ri][gi][bi] = r;
            cg->lut[1][ri][gi][bi] = g;
            cg->lut[2][ri][gi][bi] = b;
            idx++;
        }
    }
    fclose(f);
    cg->lut_size = size;
    cg->lut_loaded = (idx > 0) ? 1 : 0;
    return cg->lut_loaded ? 0 : -1;
}

/* sRGB to linear */
static float srgb_to_linear(float v) {
    return (v <= 0.04045f) ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

/* linear to sRGB */
static float linear_to_srgb(float v) {
    return (v <= 0.0031308f) ? v * 12.92f : 1.055f * powf(v, 1.0f/2.4f) - 0.055f;
}

int wb_color_grading_process(void *inst, uint8_t *rgba, int width, int height) {
    wb_color_grading *cg = (wb_color_grading *)inst;
    if (!cg || !rgba) return -1;

    for (int i = 0; i < width * height; i++) {
        int idx = i * 4;

        /* sRGB to linear */
        float r = srgb_to_linear(rgba[idx] / 255.0f);
        float g = srgb_to_linear(rgba[idx+1] / 255.0f);
        float b = srgb_to_linear(rgba[idx+2] / 255.0f);

        /* Apply LUT if loaded */
        if (cg->lut_loaded && cg->lut_size > 0) {
            float scale = (float)(cg->lut_size - 1);
            float ri = r * scale, gi = g * scale, bi = b * scale;
            int r0 = (int)ri, g0 = (int)gi, b0 = (int)bi;
            float rf = ri - r0, gf = gi - g0, bf = bi - b0;
            if (r0 >= cg->lut_size-1) { r0 = cg->lut_size-2; rf = 1.0f; }
            if (g0 >= cg->lut_size-1) { g0 = cg->lut_size-2; gf = 1.0f; }
            if (b0 >= cg->lut_size-1) { b0 = cg->lut_size-2; bf = 1.0f; }

            /* Trilinear interpolation */
            for (int c = 0; c < 3; c++) {
                float v000 = cg->lut[c][r0][g0][b0];
                float v100 = cg->lut[c][r0+1][g0][b0];
                float v010 = cg->lut[c][r0][g0+1][b0];
                float v110 = cg->lut[c][r0+1][g0+1][b0];
                float v001 = cg->lut[c][r0][g0][b0+1];
                float v101 = cg->lut[c][r0+1][g0][b0+1];
                float v011 = cg->lut[c][r0][g0+1][b0+1];
                float v111 = cg->lut[c][r0+1][g0+1][b0+1];

                float v00 = v000 * (1-rf) + v100 * rf;
                float v10 = v010 * (1-rf) + v110 * rf;
                float v01 = v001 * (1-rf) + v101 * rf;
                float v11 = v011 * (1-rf) + v111 * rf;
                float v0 = v00 * (1-gf) + v10 * gf;
                float v1 = v01 * (1-gf) + v11 * gf;
                float v = v0 * (1-bf) + v1 * bf;

                if (c == 0) r = v;
                else if (c == 1) g = v;
                else b = v;
            }
        }

        /* Lift/Gamma/Gain */
        float lift[3] = {cg->lift[0], cg->lift[1], cg->lift[2]};
        float gamma[3] = {cg->gamma[0], cg->gamma[1], cg->gamma[2]};
        float gain[3] = {cg->gain[0], cg->gain[1], cg->gain[2]};

        /* Lift: shift shadows */
        r += (lift[0] - 1.0f) * 0.1f;
        g += (lift[1] - 1.0f) * 0.1f;
        b += (lift[2] - 1.0f) * 0.1f;

        /* Gamma: midtones */
        if (r > 0) r = powf(r, 1.0f / gamma[0]);
        if (g > 0) g = powf(g, 1.0f / gamma[1]);
        if (b > 0) b = powf(b, 1.0f / gamma[2]);

        /* Gain: highlights */
        r *= gain[0];
        g *= gain[1];
        b *= gain[2];

        /* Contrast */
        r = (r - 0.5f) * cg->contrast + 0.5f;
        g = (g - 0.5f) * cg->contrast + 0.5f;
        b = (b - 0.5f) * cg->contrast + 0.5f;

        /* Saturation */
        float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        r = lum + (r - lum) * cg->saturation;
        g = lum + (g - lum) * cg->saturation;
        b = lum + (b - lum) * cg->saturation;

        /* Temperature (warm/cool) */
        r += cg->temperature * 0.1f;
        b -= cg->temperature * 0.1f;

        /* Tint (green/magenta) */
        g += cg->tint * 0.1f;

        /* Clamp and convert back to sRGB */
        r = linear_to_srgb(r < 0 ? 0 : (r > 1 ? 1 : r));
        g = linear_to_srgb(g < 0 ? 0 : (g > 1 ? 1 : g));
        b = linear_to_srgb(b < 0 ? 0 : (b > 1 ? 1 : b));

        rgba[idx]   = (uint8_t)(r * 255.0f + 0.5f);
        rgba[idx+1] = (uint8_t)(g * 255.0f + 0.5f);
        rgba[idx+2] = (uint8_t)(b * 255.0f + 0.5f);
    }

    return 0;
}
