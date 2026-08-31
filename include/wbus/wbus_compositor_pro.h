/* wbus_compositor_pro.h — professional node-based compositor (Fusion/Nuke style).
 *
 * Pull-based node graph: input/blend/key/transform/color/output nodes.
 * RGBA8 output buffer. Topological evaluation. Pure C11, zero dependencies.
 */
#ifndef WUBUS_WBUS_COMPOSITOR_PRO_H
#define WUBUS_WBUS_COMPOSITOR_PRO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque compositor handle */
typedef struct wb_compositor_pro wb_compositor_pro;

/* Node types */
enum {
    WB_NODE_INPUT       = 0,  /* solid color or gradient source            */
    WB_NODE_BLEND       = 1,  /* normal/multiply/screen/overlay/add/subtract */
    WB_NODE_KEY         = 2,  /* chroma key with tolerance                 */
    WB_NODE_TRANSFORM   = 3,  /* translate / scale / rotate                */
    WB_NODE_COLOR       = 4,  /* brightness / contrast / saturation        */
    WB_NODE_OUTPUT      = 5  /* final RGBA buffer                          */
};

/* Blend modes (set via node parameters) */
enum {
    WB_BLEND_NORMAL   = 0,
    WB_BLEND_MULTIPLY = 1,
    WB_BLEND_SCREEN   = 2,
    WB_BLEND_OVERLAY  = 3,
    WB_BLEND_ADD      = 4,
    WB_BLEND_SUBTRACT = 5
};

/* Port indices for connections */
#define WB_PORT_INPUT0    0   /* first input (also used by 2-input blend)  */
#define WB_PORT_INPUT1    1   /* second input (blend, key)                 */
#define WB_PORT_OUTPUT    0   /* output port                              */

/* ---- lifecycle ---- */

/* Create a compositor for the given dimensions. Returns NULL on failure. */
void *wb_compositor_pro_create(int width, int height);

/* Destroy a compositor created with wb_compositor_pro_create. */
void   wb_compositor_pro_destroy(void *c);

/* ---- graph construction ---- */

/* Add a node of the given type. Returns node id (>=0) or -1 on failure.
 * Node ids are small integers (0,1,2,...). */
int wb_compositor_pro_add_node(void *c, int type);

/* Connect output port `from_port` of node `from` to input port `to_port`
 * of node `to`. Returns 0 on success, -1 on failure. */
int wb_compositor_pro_connect(void *c, int from, int from_port,
                              int to, int to_port);

/* ---- node parameter setters (set before process) ---- */

/* Input node params */
void wb_comp_pro_set_solid_color(void *c, int node,
                                 float r, float g, float b, float a);
void wb_comp_pro_set_gradient(void *c, int node,
                              float r0, float g0, float b0, float a0,
                              float r1, float g1, float b1, float a1,
                              int vertical);

/* Blend node params */
void wb_comp_pro_set_blend_mode(void *c, int node, int mode);

/* Key node params (chroma key) */
void wb_comp_pro_set_key_color(void *c, int node, float r, float g, float b);
void wb_comp_pro_set_key_tolerance(void *c, int node, float tol);

/* Transform node params */
void wb_comp_pro_set_translate(void *c, int node, float dx, float dy); /* pixels */
void wb_comp_pro_set_scale(void *c, int node, float sx, float sy);
void wb_comp_pro_set_rotate(void *c, int node, float angle_deg);       /* degrees */

/* Color node params */
void wb_comp_pro_set_brightness(void *c, int node, float b);   /* -1..1 */
void wb_comp_pro_set_contrast(void *c, int node, float c);      /* 0..2  (1=identity) */
void wb_comp_pro_set_saturation(void *c, int node, float s);     /* 0..2  (1=identity) */

/* ---- evaluation ---- */

/* Process the graph and write RGBA8 into `output_rgba` (w*h*4 bytes).
 * Returns 0 on success, -1 on failure. */
int wb_compositor_pro_process(void *c, uint8_t *output_rgba);

/* ---- queries ---- */

/* Number of nodes currently in the graph. */
int wb_compositor_pro_get_node_count(const void *c);

/* Width / height of the compositor. */
int wb_compositor_pro_get_width(const void *c);
int wb_compositor_pro_get_height(const void *c);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_COMPOSITOR_PRO_H */
