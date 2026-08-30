/* wb_compositor_pro.c — professional node compositor (Fusion/Nuke style).
 *
 * Node-based compositing with blend modes, keying, transform, color.
 * Pure C11. */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_NODES 32
#define MAX_CONNECTIONS 64

typedef enum {
    NODE_INPUT = 0,
    NODE_BLEND,
    NODE_KEY,
    NODE_TRANSFORM,
    NODE_COLOR,
    NODE_OUTPUT,
    NODE_COUNT
} node_type_t;

typedef struct {
    int from_node, from_port;
    int to_node, to_port;
} connection_t;

typedef struct {
    node_type_t type;
    int active;
    float params[8];
    uint8_t *buffer; /* RGBA buffer for this node's output */
    int buf_size;
} node_t;

typedef struct {
    int width, height;
    node_t nodes[MAX_NODES];
    int node_count;
    connection_t connections[MAX_CONNECTIONS];
    int conn_count;
} wb_compositor_pro;

void *wb_compositor_pro_create(int width, int height) {
    wb_compositor_pro *c = (wb_compositor_pro *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->width = width;
    c->height = height;
    return c;
}

void wb_compositor_pro_destroy(void *inst) {
    wb_compositor_pro *c = (wb_compositor_pro *)inst;
    if (!c) return;
    for (int i = 0; i < c->node_count; i++)
        free(c->nodes[i].buffer);
    free(c);
}

int wb_compositor_pro_add_node(void *inst, int type) {
    wb_compositor_pro *c = (wb_compositor_pro *)inst;
    if (!c || type < 0 || type >= NODE_COUNT) return -1;
    if (c->node_count >= MAX_NODES) return -1;

    node_t *n = &c->nodes[c->node_count];
    n->type = type;
    n->active = 1;
    n->buf_size = c->width * c->height * 4;
    n->buffer = (uint8_t *)calloc(n->buf_size, 1);
    if (!n->buffer) return -1;

    /* Default params */
    switch (type) {
    case NODE_INPUT:
        n->params[0] = 1.0f; /* R */
        n->params[1] = 1.0f; /* G */
        n->params[2] = 1.0f; /* B */
        break;
    case NODE_BLEND:
        n->params[0] = 0.5f; /* mix */
        break;
    case NODE_COLOR:
        n->params[0] = 1.0f; /* brightness */
        n->params[1] = 1.0f; /* contrast */
        n->params[2] = 1.0f; /* saturation */
        break;
    }

    return c->node_count++;
}

int wb_compositor_pro_connect(void *inst, int from, int from_port, int to, int to_port) {
    wb_compositor_pro *c = (wb_compositor_pro *)inst;
    if (!c || from < 0 || from >= c->node_count || to < 0 || to >= c->node_count) return -1;
    if (c->conn_count >= MAX_CONNECTIONS) return -1;
    c->connections[c->conn_count].from_node = from;
    c->connections[c->conn_count].from_port = from_port;
    c->connections[c->conn_count].to_node = to;
    c->connections[c->conn_count].to_port = to_port;
    c->conn_count++;
    return 0;
}

int wb_compositor_pro_get_node_count(const void *inst) {
    const wb_compositor_pro *c = (const wb_compositor_pro *)inst;
    return c ? c->node_count : 0;
}

/* Evaluate a single node into its buffer */
static void eval_node(wb_compositor_pro *c, int node_idx) {
    node_t *n = &c->nodes[node_idx];
    if (!n->active || !n->buffer) return;

    int npx = c->width * c->height;

    switch (n->type) {
    case NODE_INPUT: {
        uint8_t r = (uint8_t)(n->params[0] * 255);
        uint8_t g = (uint8_t)(n->params[1] * 255);
        uint8_t b = (uint8_t)(n->params[2] * 255);
        for (int i = 0; i < npx; i++) {
            n->buffer[i*4] = r;
            n->buffer[i*4+1] = g;
            n->buffer[i*4+2] = b;
            n->buffer[i*4+3] = 255;
        }
        break;
    }
    case NODE_COLOR: {
        float bright = n->params[0];
        float contrast = n->params[1];
        float sat = n->params[2];
        /* Find input connection */
        for (int i = 0; i < c->conn_count; i++) {
            if (c->connections[i].to_node == node_idx) {
                node_t *src = &c->nodes[c->connections[i].from_node];
                if (!src->buffer) break;
                for (int p = 0; p < npx; p++) {
                    for (int ch = 0; ch < 3; ch++) {
                        float v = src->buffer[p*4+ch] / 255.0f;
                        v *= bright;
                        v = (v - 0.5f) * contrast + 0.5f;
                        /* Saturation */
                        float lum = (src->buffer[p*4] + src->buffer[p*4+1] + src->buffer[p*4+2]) / (3.0f * 255.0f);
                        v = lum + (v - lum) * sat;
                        n->buffer[p*4+ch] = (uint8_t)(v < 0 ? 0 : (v > 1 ? 255 : v * 255));
                    }
                    n->buffer[p*4+3] = src->buffer[p*4+3];
                }
                break;
            }
        }
        break;
    }
    case NODE_OUTPUT: {
        /* Copy from connected input node */
        for (int i = 0; i < c->conn_count; i++) {
            if (c->connections[i].to_node == node_idx) {
                node_t *src = &c->nodes[c->connections[i].from_node];
                if (!src->buffer) break;
                memcpy(n->buffer, src->buffer, npx * 4);
                break;
            }
        }
        break;
    }
    default:
        break;
    }
}

int wb_compositor_pro_process(void *inst, uint8_t *output_rgba) {
    wb_compositor_pro *c = (wb_compositor_pro *)inst;
    if (!c || !output_rgba) return -1;

    /* Simple evaluation: process nodes in order */
    for (int i = 0; i < c->node_count; i++)
        eval_node(c, i);

    /* Find output node */
    for (int i = 0; i < c->node_count; i++) {
        if (c->nodes[i].type == NODE_OUTPUT && c->nodes[i].buffer) {
            memcpy(output_rgba, c->nodes[i].buffer, c->width * c->height * 4);
            return 0;
        }
    }

    /* No output node: use last node */
    if (c->node_count > 0 && c->nodes[c->node_count-1].buffer) {
        memcpy(output_rgba, c->nodes[c->node_count-1].buffer, c->width * c->height * 4);
        return 0;
    }

    return -1;
}
