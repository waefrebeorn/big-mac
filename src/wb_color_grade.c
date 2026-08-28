/* wb_color_grade.c — color grading pipeline (LUTs, curves, wheels).
 *
 * R077: Professional color grading for video.
 *
 * Pipeline:
 *   1. Lift/Gamma/Gain (shadows/midtones/highlights)
 *   2. Curves (per-channel tone curves)
 *   3. 3D LUT application
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define CURVE_POINTS 16
#define LUT_SIZE 33

typedef struct {
    float x, y;
} curve_point_t;

typedef struct {
    /* Lift/Gamma/Gain */
    float    lift_r, lift_g, lift_b;      /* Shadow offset (-1..1) */
    float    gamma_r, gamma_g, gamma_b;   /* Midtone power (0.1..4) */
    float    gain_r, gain_g, gain_b;      /* Highlight multiplier (0..2) */

    /* Curves */
    curve_point_t curves[3][CURVE_POINTS]; /* R, G, B curves */
    int           num_curve_points[3];

    /* LUT */
    float    lut[3][LUT_SIZE];             /* 1D LUT per channel */
    int      lut_loaded;

    /* Global */
    float    contrast;    /* 0.5..2.0 */
    float    saturation;  /* 0.0..2.0 */
} wb_color_grade_inst;

void *wb_color_grade_create(void) {
    wb_color_grade_inst *cg = (wb_color_grade_inst *)calloc(1, sizeof(*cg));
    if (!cg) return NULL;

    /* Defaults */
    cg->gamma_r = cg->gamma_g = cg->gamma_b = 1.0f;
    cg->gain_r = cg->gain_g = cg->gain_b = 1.0f;
    cg->contrast = 1.0f;
    cg->saturation = 1.0f;

    /* Initialize curves as identity */
    for (int c = 0; c < 3; c++) {
        cg->num_curve_points[c] = 2;
        cg->curves[c][0].x = 0.0f;
        cg->curves[c][0].y = 0.0f;
        cg->curves[c][1].x = 1.0f;
        cg->curves[c][1].y = 1.0f;
    }

    /* Initialize LUT as identity */
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < LUT_SIZE; i++) {
            cg->lut[c][i] = (float)i / (float)(LUT_SIZE - 1);
        }
    }

    return cg;
}

void wb_color_grade_destroy(void *inst) { free(inst); }

void wb_color_grade_set(void *inst, int param, float v) {
    wb_color_grade_inst *cg = (wb_color_grade_inst *)inst;
    if (!cg) return;
    switch (param) {
    case 0: cg->lift_r = v; break;
    case 1: cg->lift_g = v; break;
    case 2: cg->lift_b = v; break;
    case 3: cg->gamma_r = v > 0.1f ? v : 0.1f; break;
    case 4: cg->gamma_g = v > 0.1f ? v : 0.1f; break;
    case 5: cg->gamma_b = v > 0.1f ? v : 0.1f; break;
    case 6: cg->gain_r = v > 0 ? v : 0; break;
    case 7: cg->gain_g = v > 0 ? v : 0; break;
    case 8: cg->gain_b = v > 0 ? v : 0; break;
    case 9: cg->contrast = v > 0 ? v : 0.1f; break;
    case 10: cg->saturation = v > 0 ? v : 0; break;
    default: break;
    }
}

/* Apply lift/gamma/gain to a single channel value. */
static float lift_gamma_gain(float x, float lift, float gamma, float gain) {
    /* Apply gain (highlights) */
    x *= gain;
    /* Apply gamma (midtones) */
    if (x > 0.0f && gamma != 1.0f) {
        x = powf(x, 1.0f / gamma);
    }
    /* Apply lift (shadows) */
    x += lift;
    return x;
}

/* Interpolate curve. */
static float curve_interp(curve_point_t *points, int num_points, float x) {
    if (num_points < 2) return x;
    if (x <= points[0].x) return points[0].y;
    if (x >= points[num_points - 1].x) return points[num_points - 1].y;

    for (int i = 0; i < num_points - 1; i++) {
        if (x >= points[i].x && x <= points[i + 1].x) {
            float t = (x - points[i].x) / (points[i + 1].x - points[i].x);
            return points[i].y + t * (points[i + 1].y - points[i].y);
        }
    }
    return x;
}

/* Apply LUT. */
static float lut_apply(float *lut, float x) {
    if (x <= 0.0f) return lut[0];
    if (x >= 1.0f) return lut[LUT_SIZE - 1];
    float idx_f = x * (float)(LUT_SIZE - 1);
    int idx = (int)idx_f;
    float frac = idx_f - (float)idx;
    return lut[idx] + frac * (lut[idx + 1] - lut[idx]);
}

/* Process an RGBA frame. */
void wb_color_grade_process(void *inst, uint8_t *rgba, int width, int height) {
    wb_color_grade_inst *cg = (wb_color_grade_inst *)inst;
    if (!cg || !rgba) return;

    int n_pixels = width * height;

    for (int i = 0; i < n_pixels; i++) {
        int idx = i * 4;

        /* Normalize to 0..1 */
        float r = (float)rgba[idx] / 255.0f;
        float g = (float)rgba[idx + 1] / 255.0f;
        float b = (float)rgba[idx + 2] / 255.0f;

        /* Lift/Gamma/Gain */
        r = lift_gamma_gain(r, cg->lift_r, cg->gamma_r, cg->gain_r);
        g = lift_gamma_gain(g, cg->lift_g, cg->gamma_g, cg->gain_g);
        b = lift_gamma_gain(b, cg->lift_b, cg->gamma_b, cg->gain_b);

        /* Contrast (around 0.5) */
        if (cg->contrast != 1.0f) {
            r = (r - 0.5f) * cg->contrast + 0.5f;
            g = (g - 0.5f) * cg->contrast + 0.5f;
            b = (b - 0.5f) * cg->contrast + 0.5f;
        }

        /* Curves */
        r = curve_interp(cg->curves[0], cg->num_curve_points[0], r);
        g = curve_interp(cg->curves[1], cg->num_curve_points[1], g);
        b = curve_interp(cg->curves[2], cg->num_curve_points[2], b);

        /* LUT */
        if (cg->lut_loaded) {
            r = lut_apply(cg->lut[0], r);
            g = lut_apply(cg->lut[1], g);
            b = lut_apply(cg->lut[2], b);
        }

        /* Saturation */
        if (cg->saturation != 1.0f) {
            float gray = 0.299f * r + 0.587f * g + 0.114f * b;
            r = gray + (r - gray) * cg->saturation;
            g = gray + (g - gray) * cg->saturation;
            b = gray + (b - gray) * cg->saturation;
        }

        /* Clamp and store */
        rgba[idx]     = (uint8_t)(r > 1.0f ? 255 : (r < 0 ? 0 : (int)(r * 255)));
        rgba[idx + 1] = (uint8_t)(g > 1.0f ? 255 : (g < 0 ? 0 : (int)(g * 255)));
        rgba[idx + 2] = (uint8_t)(b > 1.0f ? 255 : (b < 0 ? 0 : (int)(b * 255)));
    }
}
