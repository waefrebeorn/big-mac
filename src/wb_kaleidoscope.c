/* wb_kaleidoscope.c — kaleidoscope / mirror effect for YTP trippiness.
 *
 * R80: Classic YTP visual — mirror/rotate segments of the video frame
 * for psychedelic, hypnotic visuals. Also supports:
 *   - N-fold rotational symmetry
 *   - Mirror flipping
 *   - Animated rotation
 *   - Zoom pulse
 *
 * Pure C11, operates on RGBA uint8 buffers. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

typedef struct {
    int    segments;        /* Number of kaleidoscope wedges (2-16) */
    float  rotation;        /* Current rotation angle (radians) */
    float  rotation_speed;  /* Rotation speed (radians/frame) */
    int    mirror;          /* Enable mirror flip */
    float  zoom;            /* Zoom factor (1.0 = normal) */
    float  zoom_pulse;      /* Zoom pulse amount */
    float  zoom_speed;      /* Zoom pulse speed */
    int    cx, cy;          /* Center x, y */
    int    width, height;
} wb_kaleidoscope_inst;

void *wb_kaleidoscope_create(int width, int height) {
    wb_kaleidoscope_inst *inst = (wb_kaleidoscope_inst *)calloc(1, sizeof(wb_kaleidoscope_inst));
    if (!inst) return NULL;
    inst->segments = 6;
    inst->rotation = 0.0f;
    inst->rotation_speed = 0.02f;
    inst->mirror = 1;
    inst->zoom = 1.0f;
    inst->zoom_pulse = 0.0f;
    inst->zoom_speed = 0.05f;
    inst->cx = width / 2;
    inst->cy = height / 2;
    inst->width = width;
    inst->height = height;
    return inst;
}

void wb_kaleidoscope_destroy(void *inst) {
    free(inst);
}

void wb_kaleidoscope_set_segments(void *inst, int n) {
    wb_kaleidoscope_inst *k = (wb_kaleidoscope_inst *)inst;
    if (!k) return;
    if (n < 2) n = 2;
    if (n > 16) n = 16;
    k->segments = n;
}

void wb_kaleidoscope_set_rotation_speed(void *inst, float speed) {
    wb_kaleidoscope_inst *k = (wb_kaleidoscope_inst *)inst;
    if (k) k->rotation_speed = speed;
}

void wb_kaleidoscope_set_mirror(void *inst, int on) {
    wb_kaleidoscope_inst *k = (wb_kaleidoscope_inst *)inst;
    if (k) k->mirror = on ? 1 : 0;
}

void wb_kaleidoscope_set_zoom(void *inst, float zoom) {
    wb_kaleidoscope_inst *k = (wb_kaleidoscope_inst *)inst;
    if (!k) return;
    if (zoom < 0.1f) zoom = 0.1f;
    if (zoom > 5.0f) zoom = 5.0f;
    k->zoom = zoom;
}

void wb_kaleidoscope_set_zoom_pulse(void *inst, float amount, float speed) {
    wb_kaleidoscope_inst *k = (wb_kaleidoscope_inst *)inst;
    if (!k) return;
    k->zoom_pulse = amount;
    k->zoom_speed = speed;
}

/* Process one RGBA frame in-place */
void wb_kaleidoscope_process(void *inst, uint8_t *buf, int width, int height) {
    wb_kaleidoscope_inst *k = (wb_kaleidoscope_inst *)inst;
    if (!k || !buf) return;

    int cx = width / 2;
    int cy = height / 2;
    float max_radius = sqrtf((float)(cx * cx + cy * cy));

    /* Update rotation */
    k->rotation += k->rotation_speed;
    if (k->rotation > 2.0f * M_PI) k->rotation -= 2.0f * M_PI;

    /* Update zoom pulse */
    float current_zoom = k->zoom;
    if (k->zoom_pulse > 0.0f) {
        current_zoom += sinf(k->rotation * k->zoom_speed * 10.0f) * k->zoom_pulse;
    }

    /* Temp buffer */
    uint8_t *temp = (uint8_t *)malloc(width * height * 4);
    if (!temp) return;
    memcpy(temp, buf, width * height * 4);

    float wedge_angle = 2.0f * M_PI / k->segments;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            /* Destination pixel */
            int dx = x - cx;
            int dy = y - cy;

            /* Convert to polar */
            float r = sqrtf((float)(dx * dx + dy * dy)) * current_zoom;
            float theta = atan2f((float)dy, (float)dx) - k->rotation;

            /* Fold into wedge */
            theta = fmodf(theta, wedge_angle);
            if (theta < 0) theta += wedge_angle;

            /* Mirror: fold second half */
            if (k->mirror && theta > wedge_angle / 2.0f) {
                theta = wedge_angle - theta;
            }

            /* Convert back to source coordinates */
            float src_x = r * cosf(theta) + cx;
            float src_y = r * sinf(theta) + cy;

            /* Bilinear sample */
            int sx = (int)src_x;
            int sy = (int)src_y;

            if (sx >= 0 && sx < width - 1 && sy >= 0 && sy < height - 1) {
                float fx = src_x - sx;
                float fy = src_y - sy;
                int si = (sy * width + sx) * 4;
                int di = (y * width + x) * 4;

                for (int c = 0; c < 4; c++) {
                    float v00 = temp[si + c];
                    float v10 = temp[si + 4 + c];
                    float v01 = temp[si + width * 4 + c];
                    float v11 = temp[si + width * 4 + 4 + c];
                    float val = v00 * (1 - fx) * (1 - fy)
                              + v10 * fx * (1 - fy)
                              + v01 * (1 - fx) * fy
                              + v11 * fx * fy;
                    buf[di + c] = (uint8_t)(val < 0 ? 0 : (val > 255 ? 255 : val));
                }
            } else {
                /* Out of bounds: black */
                int di = (y * width + x) * 4;
                buf[di] = buf[di + 1] = buf[di + 2] = 0;
                buf[di + 3] = 255;
            }
        }
    }

    free(temp);
}
