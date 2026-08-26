/* wbus_graphio.h — R074: compositor graph save/load + recipes.
 *
 * Editor-chain state serialization, plus standalone recipe builds:
 *   make color <r> <g> <b> <a> <w> <h>
 *   make gain  <gain>
 *   make effect <op> <gain>
 *   make transition <op> <dur>
 *   make composite
 *   make text <string>
 *   make scene <path> [w] [h]
 *   input <slot>            (recipe_with_input only)
 *   wire <from> <to> <input_k>
 *   output <idx>
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

/* v8 (redesigned): recipe with an externally-provided input node.
 *
 * OWNERSHIP CONTRACT (the fix for the earlier hang):
 *   - result->owned[] lists every node the builder created; the caller
 *     destroys each via wb_node_destroy
 *   - `input` is NEVER owned or destroyed by the builder
 *   - destroy order: owned nodes first, caller's input last
 * Grammar adds:  input <slot>
 * Returns 0 on success; negative line number on parse/build error. */
typedef struct {
    wb_node *root;
    wb_node *owned[16];
    int      n_owned;
} wb_graph_recipe_result;

int wb_graphio_recipe_with_input(const char *path, wb_node *input,
                                 wb_graph_recipe_result *out);

#endif /* WUBUS_GRAPHIO_H */
