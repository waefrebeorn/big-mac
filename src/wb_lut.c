/* wb_lut.c — 3D LUT import/export (.cube format).
 *
 * R077: Professional color grading LUT support.
 *
 * Supports: .cube (Adobe/Resolve), 1D and 3D LUTs
 * Algorithm: trilinear interpolation for application
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "wbus.h"

#define LUT_MAX_SIZE 65

typedef struct {
    int      size;            /* Grid size (typically 17 or 33) */
    float    *data;           /* size*size*size*3 floats (RGB) */
    float    domain_min[3];
    float    domain_max[3];
    char     title[128];
    int      loaded;
} wb_lut3d;

typedef struct {
    int      size;            /* Typically 256 or 4096 */
    float    curve[3][65536]; /* Per-channel 1D LUT */
    int      loaded;
} wb_lut1d;

void *wb_lut_create(void) {
    wb_lut3d *lut = (wb_lut3d *)calloc(1, sizeof(wb_lut3d));
    if (!lut) return NULL;
    lut->size = 0;
    lut->loaded = 0;
    lut->domain_min[0] = lut->domain_min[1] = lut->domain_min[2] = 0.0f;
    lut->domain_max[0] = lut->domain_max[1] = lut->domain_max[2] = 1.0f;
    return lut;
}

void wb_lut_destroy(void *inst) {
    wb_lut3d *lut = (wb_lut3d *)inst;
    if (lut) { free(lut->data); free(lut); }
}

/* Load a .cube file. Returns 0 on success. */
int wb_lut_load_cube(void *inst, const char *path) {
    wb_lut3d *lut = (wb_lut3d *)inst;
    if (!lut) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    int size = 0;
    int data_count = 0;

    /* Parse header */
    while (fgets(line, sizeof(line), f)) {
        /* Skip comments */
        if (line[0] == '#') continue;

        /* Parse TITLE */
        if (strncmp(line, "TITLE", 5) == 0) {
            char *start = strchr(line, '"');
            if (start) {
                char *end = strchr(start + 1, '"');
                if (end) {
                    int len = (int)(end - start - 1);
                    if (len > 127) len = 127;
                    strncpy(lut->title, start + 1, len);
                    lut->title[len] = '\0';
                }
            }
        }
        /* Parse LUT_3D_SIZE */
        else if (strncmp(line, "LUT_3D_SIZE", 11) == 0) {
            sscanf(line + 11, "%d", &size);
        }
        /* Parse DOMAIN_MIN */
        else if (strncmp(line, "DOMAIN_MIN", 10) == 0) {
            sscanf(line + 10, "%f %f %f",
                   &lut->domain_min[0], &lut->domain_min[1], &lut->domain_min[2]);
        }
        /* Parse DOMAIN_MAX */
        else if (strncmp(line, "DOMAIN_MAX", 10) == 0) {
            sscanf(line + 10, "%f %f %f",
                   &lut->domain_max[0], &lut->domain_max[1], &lut->domain_max[2]);
        }
        /* Parse data lines */
        else if (size > 0 && data_count < size * size * size) {
            float r, g, b;
            if (sscanf(line, "%f %f %f", &r, &g, &b) == 3) {
                int needed = size * size * size * 3;
                if (!lut->data) {
                    lut->data = (float *)calloc(needed, sizeof(float));
                }
                /* .cube format: B-G-R index order (blue varies fastest) */
                lut->data[data_count * 3] = r;
                lut->data[data_count * 3 + 1] = g;
                lut->data[data_count * 3 + 2] = b;
                data_count++;
            }
        }
    }

    fclose(f);

    if (size > 0 && data_count == size * size * size) {
        lut->size = size;
        lut->loaded = 1;
        return 0;
    }

    return -1;
}

/* Save a .cube file. Returns 0 on success. */
int wb_lut_save_cube(void *inst, const char *path) {
    wb_lut3d *lut = (wb_lut3d *)inst;
    if (!lut || !lut->loaded) return -1;

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# Big Mac DAW LUT export\n");
    fprintf(f, "TITLE \"%s\"\n", lut->title);
    fprintf(f, "LUT_3D_SIZE %d\n", lut->size);
    fprintf(f, "DOMAIN_MIN %.6f %.6f %.6f\n",
            lut->domain_min[0], lut->domain_min[1], lut->domain_min[2]);
    fprintf(f, "DOMAIN_MAX %.6f %.6f %.6f\n",
            lut->domain_max[0], lut->domain_max[1], lut->domain_max[2]);

    for (int i = 0; i < lut->size * lut->size * lut->size; i++) {
        fprintf(f, "%.6f %.6f %.6f\n",
                lut->data[i * 3], lut->data[i * 3 + 1], lut->data[i * 3 + 2]);
    }

    fclose(f);
    return 0;
}

/* Apply 3D LUT to an RGBA frame using trilinear interpolation. */
void wb_lut_apply(void *inst, uint8_t *rgba, int width, int height) {
    wb_lut3d *lut = (wb_lut3d *)inst;
    if (!lut || !lut->loaded || !rgba) return;

    int n_pixels = width * height;
    float inv_size = 1.0f / (float)(lut->size - 1);

    for (int i = 0; i < n_pixels; i++) {
        int idx = i * 4;

        /* Normalize to 0..1 */
        float r = (float)rgba[idx] / 255.0f;
        float g = (float)rgba[idx + 1] / 255.0f;
        float b = (float)rgba[idx + 2] / 255.0f;

        /* Scale to LUT domain */
        r = r * (lut->domain_max[0] - lut->domain_min[0]) + lut->domain_min[0];
        g = g * (lut->domain_max[1] - lut->domain_min[1]) + lut->domain_min[1];
        b = b * (lut->domain_max[2] - lut->domain_min[2]) + lut->domain_min[2];

        /* Clamp */
        if (r < 0) r = 0; if (r > 1) r = 1;
        if (g < 0) g = 0; if (g > 1) g = 1;
        if (b < 0) b = 0; if (b > 1) b = 1;

        /* Compute indices */
        float rf = r * (float)(lut->size - 1);
        float gf = g * (float)(lut->size - 1);
        float bf = b * (float)(lut->size - 1);

        int r0 = (int)rf;
        int g0 = (int)gf;
        int b0 = (int)bf;

        if (r0 >= lut->size - 1) r0 = lut->size - 2;
        if (g0 >= lut->size - 1) g0 = lut->size - 2;
        if (b0 >= lut->size - 1) b0 = lut->size - 2;

        float fr = rf - (float)r0;
        float fg = gf - (float)g0;
        float fb = bf - (float)b0;

        /* Trilinear interpolation */
        int stride = lut->size * lut->size;
        int slice = lut->size;

        #define LUT_AT(r,g,b) \
            (&lut->data[((r)*stride + (g)*slice + (b)) * 3])

        float *c000 = LUT_AT(r0, g0, b0);
        float *c100 = LUT_AT(r0+1, g0, b0);
        float *c010 = LUT_AT(r0, g0+1, b0);
        float *c110 = LUT_AT(r0+1, g0+1, b0);
        float *c001 = LUT_AT(r0, g0, b0+1);
        float *c101 = LUT_AT(r0+1, g0, b0+1);
        float *c011 = LUT_AT(r0, g0+1, b0+1);
        float *c111 = LUT_AT(r0+1, g0+1, b0+1);

        float out_r = 0, out_g = 0, out_b = 0;
        float w;

        /* Interpolate along R */
        for (int c = 0; c < 3; c++) {
            float c00 = c000[c] * (1-fr) + c100[c] * fr;
            float c01 = c010[c] * (1-fr) + c110[c] * fr;
            float c10 = c001[c] * (1-fr) + c101[c] * fr;
            float c11 = c011[c] * (1-fr) + c111[c] * fr;

            /* Interpolate along G */
            float c0 = c00 * (1-fg) + c01 * fg;
            float c1 = c10 * (1-fg) + c11 * fg;

            /* Interpolate along B */
            float result = c0 * (1-fb) + c1 * fb;

            if (c == 0) out_r = result;
            else if (c == 1) out_g = result;
            else out_b = result;
        }

        /* Store */
        rgba[idx]     = (uint8_t)(out_r > 1.0f ? 255 : (out_r < 0 ? 0 : (int)(out_r * 255)));
        rgba[idx + 1] = (uint8_t)(out_g > 1.0f ? 255 : (out_g < 0 ? 0 : (int)(out_g * 255)));
        rgba[idx + 2] = (uint8_t)(out_b > 1.0f ? 255 : (out_b < 0 ? 0 : (int)(out_b * 255)));
    }
}

/* Generate a simple identity LUT. */
void wb_lut_generate_identity(void *inst, int size) {
    wb_lut3d *lut = (wb_lut3d *)inst;
    if (!lut || size < 2 || size > LUT_MAX_SIZE) return;

    if (lut->data) free(lut->data);
    lut->data = (float *)calloc(size * size * size * 3, sizeof(float));
    lut->size = size;
    lut->loaded = 1;

    for (int r = 0; r < size; r++) {
        for (int g = 0; g < size; g++) {
            for (int b = 0; b < size; b++) {
                int idx = ((r * size * size) + (g * size) + b) * 3;
                lut->data[idx]     = (float)r / (float)(size - 1);
                lut->data[idx + 1] = (float)g / (float)(size - 1);
                lut->data[idx + 2] = (float)b / (float)(size - 1);
            }
        }
    }
}
