/* wb_null_object.c — null objects + parenting for transform hierarchy
 * R089: After Effects parity — null objects and layer parenting
 *
 * A null object is a transform-only node with no visual output.
 * It can be parented to other nodes, creating a transform hierarchy.
 * Child nodes inherit their parent's transform (position, scale, rotation).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_compositor.h"

/* ---- Transform hierarchy ---- */

typedef struct wb_transform {
    float x, y;          /* position offset */
    float scale_x, scale_y; /* scale */
    float rotation;      /* degrees */
    float anchor_x, anchor_y; /* pivot point */
    float opacity;       /* 0.0 - 1.0 */
} wb_transform;

/* ---- Null object node ---- */

typedef struct {
    wb_transform transform;
    wb_node *parent;     /* parent node (NULL = root) */
    wb_node **children;  /* child nodes */
    int child_count;
    int child_cap;
} wb_null_data;

static wb_frame *null_node_pull(wb_node *node, double t, int rx, int ry, int rw, int rh, int phase) {
    /* Null objects produce no visual output — return NULL */
    (void)node; (void)t; (void)rx; (void)ry; (void)rw; (void)rh; (void)phase;
    return NULL;
}

static void null_node_free(wb_node *node) {
    wb_null_data *nd = (wb_null_data *)node->user;
    if (!nd) return;
    free(nd->children);
    free(nd);
    node->user = NULL;
}

wb_node *wb_node_create_null(const char *name) {
    wb_null_data *nd = (wb_null_data *)calloc(1, sizeof(wb_null_data));
    if (!nd) return NULL;

    nd->transform.scale_x = 1.0f;
    nd->transform.scale_y = 1.0f;
    nd->transform.opacity = 1.0f;
    nd->child_cap = 8;
    nd->children = (wb_node **)calloc(nd->child_cap, sizeof(wb_node *));
    nd->child_count = 0;
    nd->parent = NULL;

    wb_node *node = (wb_node *)calloc(1, sizeof(wb_node));
    node->kind = WB_NODE_COMPOSITE; /* composite type for transform nodes */
    snprintf(node->id, sizeof(node->id), "null_%s", name ? name : "obj");
    node->n_inputs = 0;
    node->inputs = NULL;
    node->user = nd;
    node->pull = null_node_pull;
    node->free = null_node_free;
    node->fmt_w = 0;
    node->fmt_h = 0;
    node->is_identity = 1;
    node->params = NULL;
    node->owns_params = 0;
    node->param_lanes = NULL;
    return node;
}

/* ---- Parenting API ---- */

void wb_node_set_parent(wb_node *child, wb_node *parent) {
    if (!child) return;

    /* Remove from old parent if any */
    wb_null_data *cd = (wb_null_data *)child->user;
    if (cd && cd->parent) {
        wb_null_data *pd = (wb_null_data *)cd->parent->user;
        if (pd) {
            for (int i = 0; i < pd->child_count; i++) {
                if (pd->children[i] == child) {
                    pd->children[i] = pd->children[--pd->child_count];
                    break;
                }
            }
        }
    }

    /* Set new parent */
    cd->parent = parent;

    /* Add to parent's children list */
    if (parent) {
        wb_null_data *pd = (wb_null_data *)parent->user;
        if (pd) {
            if (pd->child_count >= pd->child_cap) {
                pd->child_cap *= 2;
                pd->children = (wb_node **)realloc(pd->children, pd->child_cap * sizeof(wb_node *));
            }
            pd->children[pd->child_count++] = child;
        }
    }
}

wb_node *wb_node_get_parent(wb_node *child) {
    if (!child) return NULL;
    wb_null_data *cd = (wb_null_data *)child->user;
    return cd ? cd->parent : NULL;
}

int wb_node_get_child_count(wb_node *parent) {
    if (!parent) return 0;
    wb_null_data *pd = (wb_null_data *)parent->user;
    return pd ? pd->child_count : 0;
}

wb_node *wb_node_get_child(wb_node *parent, int index) {
    if (!parent) return NULL;
    wb_null_data *pd = (wb_null_data *)parent->user;
    if (!pd || index < 0 || index >= pd->child_count) return NULL;
    return pd->children[index];
}

/* ---- Transform API ---- */

void wb_node_set_position(wb_node *node, float x, float y) {
    if (!node) return;
    wb_null_data *nd = (wb_null_data *)node->user;
    if (nd) { nd->transform.x = x; nd->transform.y = y; }
}

void wb_node_set_scale(wb_node *node, float sx, float sy) {
    if (!node) return;
    wb_null_data *nd = (wb_null_data *)node->user;
    if (nd) { nd->transform.scale_x = sx; nd->transform.scale_y = sy; }
}

void wb_node_set_rotation(wb_node *node, float degrees) {
    if (!node) return;
    wb_null_data *nd = (wb_null_data *)node->user;
    if (nd) { nd->transform.rotation = degrees; }
}

void wb_node_set_opacity(wb_node *node, float opacity) {
    if (!node) return;
    wb_null_data *nd = (wb_null_data *)node->user;
    if (nd) { nd->transform.opacity = opacity < 0 ? 0 : opacity > 1 ? 1 : opacity; }
}

void wb_node_get_position(wb_node *node, float *x, float *y) {
    if (!node) return;
    wb_null_data *nd = (wb_null_data *)node->user;
    if (nd) { if (x) *x = nd->transform.x; if (y) *y = nd->transform.y; }
}

void wb_node_get_scale(wb_node *node, float *sx, float *sy) {
    if (!node) return;
    wb_null_data *nd = (wb_null_data *)node->user;
    if (nd) { if (sx) *sx = nd->transform.scale_x; if (sy) *sy = nd->transform.scale_y; }
}

float wb_node_get_rotation(wb_node *node) {
    if (!node) return 0.0f;
    wb_null_data *nd = (wb_null_data *)node->user;
    return nd ? nd->transform.rotation : 0.0f;
}

float wb_node_get_opacity(wb_node *node) {
    if (!node) return 1.0f;
    wb_null_data *nd = (wb_null_data *)node->user;
    return nd ? nd->transform.opacity : 1.0f;
}

/* ---- Computed transform (with parent inheritance) ---- */

void wb_node_get_world_transform(wb_node *node, float *out_x, float *out_y,
                                   float *out_sx, float *out_sy, float *out_rot) {
    if (!node) return;

    float x = 0, y = 0, sx = 1, sy = 1, rot = 0;

    /* Walk up the hierarchy, accumulating transforms */
    wb_node *cur = node;
    while (cur) {
        wb_null_data *nd = (wb_null_data *)cur->user;
        if (nd) {
            x += nd->transform.x;
            y += nd->transform.y;
            sx *= nd->transform.scale_x;
            sy *= nd->transform.scale_y;
            rot += nd->transform.rotation;
        }
        cur = cur->user ? ((wb_null_data *)cur->user)->parent : NULL;
    }

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_sx) *out_sx = sx;
    if (out_sy) *out_sy = sy;
    if (out_rot) *out_rot = rot;
}
