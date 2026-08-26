/* wb_graphio.c — R074 hop 191 (G-SF080): compositor node-graph
 * save/load. Serializes the editable graph state: labels, layout,
 * connections; load restores them via the graph mutators.
 * Pure C11, stdio only. */
#include "wbus/wbus_graphio.h"
#include <stdio.h>
#include <string.h>

int wb_graphio_save(const wb_node_graph *g, const char *path) {
    if (!g || !path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# big-mac node graph\n");
    int n = wb_node_graph_count(g);
    for (int i = 0; i < n; i++) {
        float x, y;
        wb_node_graph_pos(g, i, &x, &y);
        fprintf(f, "node %d %d \"%s\"\n", i,
                (int)wb_node_graph_kind(g, i),
                wb_node_graph_label(g, i));
        fprintf(f, "layout %d %.2f %.2f\n", i, x, y);
        for (int k = 0; k < wb_node_graph_inputs(g, i); k++) {
            int src = wb_node_graph_input_of(g, i, k);
            if (src >= 0)
                fprintf(f, "conn %d %d %d\n", src, i, k);
        }
    }
    fclose(f);
    return 0;
}

int wb_graphio_load(wb_node_graph *g, const char *path) {
    if (!g || !path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p) continue;
        char kw[16];
        if (sscanf(p, "%15s", kw) != 1) continue;
        if (!strcmp(kw, "layout")) {
            int idx; float x, y;
            if (sscanf(p, "layout %d %f %f", &idx, &x, &y) == 3)
                wb_node_graph_set_pos(g, idx, x, y);
        } else if (!strcmp(kw, "conn")) {
            int from, to, k;
            if (sscanf(p, "conn %d %d %d", &from, &to, &k) == 3)
                wb_node_graph_connect(g, from, to, k);
        }
    }
    fclose(f);
    return 0;
}
