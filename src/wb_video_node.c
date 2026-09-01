/* wb_video_node.c — compositor source node wrapping wb_video_decoder.
 *
 * A WB_NODE_SOURCE that decodes a video file on pull: seeks to the requested
 * time (accounting for start_in_source offset), decodes one frame to RGBA via
 * wb_video_decoder, converts uint8 RGBA -> float wb_px, and returns a
 * wb_frame. Output dimensions default to PROXY_SCALE_W/H (854x480) but are
 * caller-overridable for full-res pulls.
 *
 * Pattern follows src_frame_pull in wb_compositor.c: wb_node_create +
 * set n->user/n->pull/n->free + wb_node_set_format. */

#include "wbus/wbus_compositor.h"
#include "wbus/wbus_video.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* PROXY_SCALE_W/H live in wb_video.c, not wbus_video.h — replicate the
 * canonical proxy dimensions here so the header stays the single source
 * of truth if they're ever moved. */
#ifndef PROXY_SCALE_W
#define PROXY_SCALE_W 854
#endif
#ifndef PROXY_SCALE_H
#define PROXY_SCALE_H 480
#endif

typedef struct {
    wb_video_decoder *decoder;
    char              path[512];
    double            start_in_source;  /* seconds into source to start */
    double            duration;         /* seconds to play (0 = all) */
    int               w, h;             /* output dimensions (proxy/frame) */
    int               eof;              /* set after decode returns -1 */
} src_video_t;

static wb_frame *src_video_pull(wb_node *self, double t,
                                int rx, int ry, int rw, int rh, int phase) {
    (void)rx; (void)ry; (void)rw; (void)rh;
    if (phase == 0) return NULL;   /* source is synchronous; nothing to request */

    src_video_t *s = self->user;
    if (!s || !s->decoder) return NULL;

    /* Map compositor time (relative to clip start) to source time. */
    double source_time = t + s->start_in_source;

    /* Clamp to [0, duration) when duration is known. */
    if (s->duration > 0.0 && source_time >= s->duration) {
        source_time = s->duration - 0.001;  /* stay inside last frame */
    }
    if (source_time < 0.0) source_time = 0.0;

    /* Seek + decode one frame into a temp RGBA buffer. */
    int seek_rc = wb_video_decoder_seek(s->decoder, source_time);
    if (seek_rc < 0) {
        /* Seek failed — return a black frame rather than NULL so the
         * compositor doesn't tear down the graph on a single bad seek. */
        s->eof = 1;
    }

    int out_w = s->w, out_h = s->h;
    uint8_t *rgba = NULL;
    if (!s->eof) {
        /* Allocate a temp RGBA buffer for the decoder to write into. */
        rgba = calloc((size_t)out_w * out_h * 4, 1);
        if (!rgba) return NULL;

        int dec_rc = wb_video_decoder_decode_frame(s->decoder, rgba, &out_w, &out_h);
        if (dec_rc < 0) {
            /* EOF or decode error — serve a black frame on EOF, free rgba. */
            free(rgba);
            rgba = NULL;
            s->eof = 1;
        }
    }

    /* Allocate the output wb_frame (always succeed so downstream has pixels). */
    wb_frame *f = wb_frame_alloc(s->w, s->h);
    if (!f) {
        free(rgba);
        return NULL;
    }

    /* Convert uint8 RGBA -> float wb_px (divide by 255.0f). */
    if (rgba) {
        int n = s->w * s->h;
        for (int i = 0; i < n; i++) {
            wb_px *q = &f->px[i];
            q->r = rgba[i * 4 + 0] / 255.0f;
            q->g = rgba[i * 4 + 1] / 255.0f;
            q->b = rgba[i * 4 + 2] / 255.0f;
            q->a = rgba[i * 4 + 3] / 255.0f;
        }
        free(rgba);
    }
    /* else: frame is already zeroed (calloc) = transparent black */

    /* Set ROI to full frame. */
    f->roi_x = 0; f->roi_y = 0; f->roi_w = s->w; f->roi_h = s->h;
    return f;
}

static void src_video_free(wb_node *n) {
    if (!n) return;
    src_video_t *s = n->user;
    if (s) {
        if (s->decoder) wb_video_decoder_close(s->decoder);
        free(s);
    }
}

/* ---- public API ------------------------------------------------------- */

wb_node *wb_node_source_video(const char *path, int proxy_w, int proxy_h) {
    if (!path || !path[0]) return NULL;

    wb_node *n = wb_node_create(WB_NODE_SOURCE, "src_video");
    if (!n) return NULL;

    src_video_t *s = calloc(1, sizeof(*s));
    if (!s) { wb_node_destroy(n); return NULL; }

    s->decoder = wb_video_decoder_open(path);
    if (!s->decoder) {
        fprintf(stderr, "wb_node_source_video: failed to open %s\n", path);
        free(s);
        wb_node_destroy(n);
        return NULL;
    }

    snprintf(s->path, sizeof(s->path), "%s", path);
    s->start_in_source = 0.0;
    s->duration = wb_video_decoder_get_duration(s->decoder);
    s->w = proxy_w > 0 ? proxy_w : PROXY_SCALE_W;
    s->h = proxy_h > 0 ? proxy_h : PROXY_SCALE_H;
    s->eof = 0;

    n->user = s;
    n->pull = src_video_pull;
    n->free = src_video_free;
    wb_node_set_format(n, s->w, s->h);
    return n;
}

double wb_node_source_video_duration(const wb_node *n) {
    if (!n) return 0.0;
    src_video_t *s = n->user;
    return s ? s->duration : 0.0;
}

int wb_node_source_video_width(const wb_node *n) {
    if (!n) return 0;
    src_video_t *s = n->user;
    return s ? s->w : 0;
}

int wb_node_source_video_height(const wb_node *n) {
    if (!n) return 0;
    src_video_t *s = n->user;
    return s ? s->h : 0;
}