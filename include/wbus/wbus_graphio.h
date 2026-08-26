/* wbus_graphio.h — R074 hop 191 (G-SF080): compositor graph
 * save/load. Text format:
 *   node <idx> <kind> "<label>"
 *   conn <from_idx> <to_idx> <input_k>
 *   layout <idx> <x> <y>
 * Comments (#) ignored. Load rebuilds the demo chain then rewrites
 * topology from the file. */
#ifndef WUBUS_GRAPHIO_H
#define WUBUS_GRAPHIO_H

#include "wbus_compositor.h"

int  wb_graphio_save(const wb_node_graph *g, const char *path);
int  wb_graphio_load(wb_node_graph *g, const char *path);

#endif
