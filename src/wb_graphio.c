/* wb_graphio.c — R074 hop 191 (G-SF080): compositor node-graph
 * save/load. Serializes the editable graph state: labels, layout,
 * connections; load restores them via the graph mutators.
 * Pure C11, stdio only. */
#include "wbus/wbus_graphio.h"
#include "wbus/wbus_param_track.h"
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
        int np = wb_node_graph_param_count(g, i);
        for (int pi = 0; pi < np; pi++) {
            const char *pn = wb_node_graph_param_name(g, i, pi);
            float pv = wb_node_graph_param_value(g, i, pi, 0.0);
            if (pn) fprintf(f, "param %d %s %.4f\n", i, pn, pv);
        }
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
        } else if (!strcmp(kw, "param")) {
            /* G-SF080 v2: restore param values — bind a hold track. */
            int idx; char pname[32]; float val;
            if (sscanf(p, "param %d %31s %f", &idx, pname, &val) == 3) {
                wb_param_track *tr = wb_param_track_create();
                if (tr) {
                    wb_param_track_set(tr, 0.0, val, WB_KF_HOLD);
                    wb_node_graph_bind_param(g, idx, pname, tr);
                }
            }
        }
    }
    fclose(f);
    return 0;
}
