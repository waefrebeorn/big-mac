/* wb_proxy.c — proxy editing for 4K video on dual-core.
 *
 * R078 H4: Low-res preview, full-res export.
 *
 * Algorithm:
 *   1. On import, generate low-res proxy (960x540 or 1280x720)
 *   2. Timeline uses proxy for playback
 *   3. Export switches to original full-res media
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "wbus.h"

#define PROXY_MAX_PATH 512

typedef enum {
    PROXY_STATE_NONE = 0,
    PROXY_STATE_GENERATING,
    PROXY_STATE_READY,
    PROXY_STATE_ERROR
} proxy_state_t;

typedef struct {
    char            original_path[PROXY_MAX_PATH];
    char            proxy_path[PROXY_MAX_PATH];
    int             proxy_width;
    int             proxy_height;
    int             original_width;
    int             original_height;
    proxy_state_t   state;
    int             use_proxy;      /* 1=use proxy for preview, 0=use original */
    float           proxy_scale;    /* scale factor from original to proxy */
} wb_proxy_inst;

void *wb_proxy_create(void) {
    wb_proxy_inst *px = (wb_proxy_inst *)calloc(1, sizeof(wb_proxy_inst));
    if (!px) return NULL;
    px->proxy_width = 960;
    px->proxy_height = 540;
    px->use_proxy = 1;
    px->state = PROXY_STATE_NONE;
    return px;
}

void wb_proxy_destroy(void *inst) { free(inst); }

void wb_proxy_set_quality(void *inst, int width, int height) {
    wb_proxy_inst *px = (wb_proxy_inst *)inst;
    if (!px) return;
    px->proxy_width = width > 0 ? width : 960;
    px->proxy_height = height > 0 ? height : 540;
}

/* Generate proxy for a video file.
 * Returns 0 on success, -1 on error. */
int wb_proxy_generate(void *inst, const char *original_path) {
    wb_proxy_inst *px = (wb_proxy_inst *)inst;
    if (!px || !original_path) return -1;

    strncpy(px->original_path, original_path, PROXY_MAX_PATH - 1);
    px->state = PROXY_STATE_GENERATING;

    /* Generate proxy path */
    snprintf(px->proxy_path, PROXY_MAX_PATH, "%s_proxy_%dx%d.mp4",
             original_path, px->proxy_width, px->proxy_height);

    /* Use ffmpeg to generate proxy (simplified — in production, use wb_video API) */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -i \"%s\" -vf \"scale=%d:%d\" -c:v libx264 "
             "-preset ultrafast -crf 28 -an \"%s\" >/dev/null 2>&1",
             original_path, px->proxy_width, px->proxy_height, px->proxy_path);

    int rc = system(cmd);
    if (rc != 0) {
        px->state = PROXY_STATE_ERROR;
        return -1;
    }

    px->state = PROXY_STATE_READY;
    px->use_proxy = 1;
    return 0;
}

/* Get the path to use for playback (proxy or original). */
const char* wb_proxy_get_playback_path(void *inst) {
    wb_proxy_inst *px = (wb_proxy_inst *)inst;
    if (!px) return NULL;
    if (px->use_proxy && px->state == PROXY_STATE_READY) {
        return px->proxy_path;
    }
    return px->original_path;
}

/* Get the path to use for export (always original). */
const char* wb_proxy_get_export_path(void *inst) {
    wb_proxy_inst *px = (wb_proxy_inst *)inst;
    if (!px) return NULL;
    return px->original_path;
}

/* Check if proxy is ready. */
int wb_proxy_is_ready(void *inst) {
    wb_proxy_inst *px = (wb_proxy_inst *)inst;
    return px && px->state == PROXY_STATE_READY;
}

/* Toggle proxy usage. */
void wb_proxy_set_enabled(void *inst, int enabled) {
    wb_proxy_inst *px = (wb_proxy_inst *)inst;
    if (!px) return;
    px->use_proxy = enabled;
}
