/* wbus_graphio.h — R074 hop 206+: compositor graph save/load + recipes.
 *
 * Graph state (labels/layout/connections) for the fixed editor chain,
 * plus standalone recipe builds:
 *   make color <r> <g> <b> <a> <w> <h>
 *   make gain  <gain>
 *   make effect <op> <gain>
 *   make composite
 *   wire <from_idx> <to_idx> <input_k>
 *   output <idx>
 * The last wired node marked `output` is returned as the pull root.
 */
#ifndef WUBUS_GRAPHIO_H
#define WUBUS_GRAPHIO_H

#include "wbus_compositor.h"

/* Editor-chain state serialization. */
int  wb_graphio_save(const wb_node_graph *g, const char *path);
int  wb_graphio_load(wb_node_graph *g, const char *path);

/* Recipe build: parses path and constructs a node DAG. On success
 * returns 0, sets *root to the output node (caller destroys the whole
 * chain by destroying root — composite owns children), and writes the
 * created node pointers into out_nodes[0..out_n-1] (may be NULL).
 * Returns negative line number of the first error. */
int  wb_graphio_build_recipe(const char *path, wb_node **root,
                             wb_node **out_nodes, int *out_n);

#endif
