/* wb_edit_serialize.c — edit graph save/load (R085).
 *
 * Serializes wb_edit_graph to a simple text format (.bedit).
 * Format is line-oriented, one entity per section:
 *
 *   BEDIT 1.0
 *   FPS <fps>
 *   DIM <w> <h>
 *   TRACK <idx> <name>
 *   CLIP <track> <idx> <source> <start> <dur> <tl_pos>
 *   TRANS <track> <idx> <clip_a> <type> <dur>
 *   SEQ <idx> <fps> <w> <h>
 *   END
 *
 * This is intentionally simple — a production format would use JSON or
 * binary, but this is parseable, debuggable, and C11.
 */

#include "wbus/wbus_edit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BEDIT_VERSION "1.0"

int wb_edit_graph_save(const wb_edit_graph *g, const char *path) {
    if (!g || !path) return -1;

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "BEDIT %s\n", BEDIT_VERSION);
    fprintf(f, "FPS %.4f\n", g->fps);
    fprintf(f, "DIM %d %d\n", g->width, g->height);

    for (uint32_t t = 0; t < g->track_count; t++) {
        wb_edit_track *tr = &g->tracks[t];
        fprintf(f, "TRACK %u %s\n", t, tr->name);

        for (uint32_t c = 0; c < tr->clip_count; c++) {
            wb_edit_clip *cl = &tr->clips[c];
            /* Escape backslashes and newlines in paths */
            char escaped[1024];
            int j = 0;
            for (int i = 0; cl->source_path[i] && j < 1022; i++) {
                if (cl->source_path[i] == '\\') {
                    escaped[j++] = '\\';
                    escaped[j++] = '\\';
                } else if (cl->source_path[i] == '\n') {
                    escaped[j++] = '\\';
                    escaped[j++] = 'n';
                } else {
                    escaped[j++] = cl->source_path[i];
                }
            }
            escaped[j] = '\0';
            fprintf(f, "CLIP %u %u %s %.4f %.4f %.4f\n",
                    t, c, escaped, cl->start_in_source, cl->duration, cl->timeline_pos);
        }

        for (uint32_t tr_i = 0; tr_i < tr->trans_count; tr_i++) {
            wb_edit_transition *tran = &tr->transitions[tr_i];
            fprintf(f, "TRANS %u %u %u %d %.4f\n",
                    t, tr_i, tran->clip_a_idx, tran->type, tran->duration);
        }
    }

    fprintf(f, "END\n");
    fclose(f);
    return 0;
}

wb_edit_graph *wb_edit_graph_load(const char *path) {
    if (!path) return NULL;

    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    char line[2048];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return NULL; }

    /* Check version */
    if (strncmp(line, "BEDIT ", 6) != 0) { fclose(f); return NULL; }

    double fps = 30.0;
    int w = 854, h = 480;

    /* Parse header */
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "FPS ", 4) == 0) {
            sscanf(line + 4, "%lf", &fps);
        } else if (strncmp(line, "DIM ", 4) == 0) {
            sscanf(line + 4, "%d %d", &w, &h);
        } else if (strncmp(line, "TRACK ", 6) == 0) {
            break;  /* Start of track data */
        }
    }

    wb_edit_graph *g = wb_edit_graph_create(fps, w, h);
    if (!g) { fclose(f); return NULL; }

    /* Parse tracks, clips, transitions */
    /* We already read the first TRACK line into 'line' */
    do {
        if (strncmp(line, "TRACK ", 6) == 0) {
            char name[64] = {0};
            unsigned int idx;
            sscanf(line + 6, "%u %63[^\n]", &idx, name);
            /* Add tracks up to and including idx */
            while (g->track_count <= idx) {
                wb_edit_add_track(g, "");
            }
            strncpy(g->tracks[idx].name, name, sizeof(g->tracks[idx].name) - 1);
        } else if (strncmp(line, "CLIP ", 5) == 0) {
            unsigned int t_idx, c_idx;
            char source[512];
            double start, dur, tl;
            sscanf(line + 5, "%u %u %511s %lf %lf %lf",
                   &t_idx, &c_idx, source, &start, &dur, &tl);
            /* Ensure track exists */
            while (g->track_count <= t_idx) {
                wb_edit_add_track(g, "");
            }
            if (g->tracks[t_idx].clip_count < 256) {
                wb_edit_clip *cl = &g->tracks[t_idx].clips[g->tracks[t_idx].clip_count];
                strncpy(cl->source_path, source, sizeof(cl->source_path) - 1);
                cl->start_in_source = start;
                cl->duration = dur;
                cl->timeline_pos = tl;
                cl->track = (int)t_idx;
                cl->speed = 1.0f;
                cl->gain = 1.0f;
                cl->fx_chain = NULL;
                cl->source_node = NULL;
                g->tracks[t_idx].clip_count++;
            }
        } else if (strncmp(line, "TRANS ", 6) == 0) {
            unsigned int t_idx, tr_idx, clip_a;
            int type;
            double dur;
            sscanf(line + 6, "%u %u %u %d %lf",
                   &t_idx, &tr_idx, &clip_a, &type, &dur);
            if (t_idx < g->track_count && g->tracks[t_idx].trans_count < 256) {
                wb_edit_transition *tran = &g->tracks[t_idx].transitions[g->tracks[t_idx].trans_count];
                tran->clip_a_idx = clip_a;
                tran->clip_b_idx = clip_a + 1;
                tran->type = type;
                tran->duration = dur;
                tran->trans_node = NULL;
                g->tracks[t_idx].trans_count++;
            }
        } else if (strncmp(line, "END", 3) == 0) {
            break;
        }
    } while (fgets(line, sizeof(line), f));

    fclose(f);
    return g;
}
