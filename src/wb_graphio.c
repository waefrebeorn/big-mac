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
        /* G-SF080 v3: serialize full param tracks (all keys), falling
         * back to the t=0 value when a track has keys. */
        struct wb_node *nd = wb_node_graph_node_at(g, i);
        int np = nd ? wb_node_graph_param_count(g, i) : 0;
        for (int pi = 0; pi < np; pi++) {
            const char *pn = wb_node_graph_param_name(g, i, pi);
            if (!pn || !nd || !nd->params || !nd->params[pi]) continue;
            int nk = wb_param_track_count(nd->params[pi]);
            if (nk <= 0) continue;
            fprintf(f, "ptrack %d %s %d\n", i, pn, nk);
            for (int ki = 0; ki < nk; ki++) {
                wb_keyframe kf;
                if (wb_param_track_key_index(nd->params[pi], ki, &kf) == 0)
                    fprintf(f, "key %.6f %.6f\n", kf.t, kf.value);
            }
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
    wb_param_track *pend_track = NULL;
    int pend_node = -1, pend_keys = 0, pend_got = 0;
    char pend_name[32] = "";
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
        } else if (!strcmp(kw, "ptrack")) {
            /* G-SF080 v3: rebuild a full param track from key lines. */
            int idx, nk;
            char pname[32];
            if (sscanf(p, "ptrack %d %31s %d", &idx, pname, &nk) == 3
                && nk > 0 && nk <= 4096) {
                wb_param_track *tr = wb_param_track_create();
                if (tr) {
                    pend_track = tr; pend_node = idx;
                    snprintf(pend_name, sizeof pend_name, "%s", pname);
                    pend_keys = nk; pend_got = 0;
                }
            }
        } else if (!strcmp(kw, "key") && pend_track) {
            double kt; float kv;
            if (sscanf(p, "key %lf %f", &kt, &kv) == 2) {
                wb_param_track_set(pend_track, kt, kv, WB_KF_HOLD);
                pend_got++;
                if (pend_got >= pend_keys) {
                    wb_node_graph_bind_param(g, pend_node,
                                             pend_name, pend_track);
                    pend_track = NULL;
                }
            }
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


/* ---- recipe builder (R074 hop 210, G-SF080 deep half) ----------------- */
#include <stdlib.h>

int wb_graphio_build_recipe(const char *path, wb_node **root,
                            wb_node **out_nodes, int *out_n) {
    if (!path || !root) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    enum { CAP = 16 };
    wb_node *nodes[CAP];
    int n = 0;
    char line[512];
    int lineno = 0;
    int out_idx = -1;

    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p) continue;

        char kw[16];
        if (sscanf(p, "%15s", kw) != 1) continue;

        if (!strcmp(kw, "make")) {
            char kind[24];
            float a=0,b2=0,c2=0,d2=1,e2=0,g2=0;
            int cnt = sscanf(p, "make %23s %f %f %f %f %f %f",
                             kind,&a,&b2,&c2,&d2,&e2,&g2);
            if (cnt < 2) { fclose(f); return -lineno; }
            wb_node *nd = NULL;
            if (!strcmp(kind,"color"))
                nd = wb_node_source_color(a,b2,c2,d2,
                                          cnt>4?(int)e2:64,
                                          cnt>4?(int)g2:64);
            else if (!strcmp(kind,"gain"))
                nd = wb_node_effect(1, a > 0 ? a : 1.0f);
            else if (!strcmp(kind,"composite"))
                nd = wb_node_composite();
            else if (!strcmp(kind,"effect")) {
                /* G-SF080 v4: any effect op 0..11 by number */
                int op = (int)a;
                float g2v = b2 > 0 ? b2 : 1.0f;
                if (op >= 0 && op <= 11)
                    nd = wb_node_effect(op, g2v);
            }
            if (!nd || n >= CAP) { fclose(f); return -lineno; }
            nodes[n++] = nd;
            continue;
        }
        if (!strcmp(kw, "wire")) {
            int from, to, k;
            if (sscanf(p, "wire %d %d %d", &from,&to,&k) != 3) {
                fclose(f); return -lineno;
            }
            if (from<0||from>=n||to<0||to>=n||k<0||k>=4) {
                fclose(f); return -lineno;
            }
            wb_node_connect(nodes[to], nodes[from], k);
            continue;
        }
        if (!strcmp(kw, "output")) {
            if (sscanf(p, "output %d", &out_idx) != 1) {
                fclose(f); return -lineno;
            }
            continue;
        }
        fclose(f); return -lineno;
    }
    fclose(f);
    if (out_idx < 0 || out_idx >= n) return -lineno;
    *root = nodes[out_idx];
    if (out_nodes) {
        int m = n < *out_n ? n : *out_n;
        for (int i = 0; i < m; i++) out_nodes[i] = nodes[i];
        if (out_n) *out_n = m;
    } else if (out_n) *out_n = n;
    return 0;
}
