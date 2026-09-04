/* wb_prod_project.c — YTPMV Production Project + Render Graph (R105).
 * Types defined in wbus_compositor.h — this file is implementations only.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wbus/wbus_compositor.h"

/* Media library */
void wb_media_init(wb_media_lib *lib) {
    if (!lib) return;
    memset(lib, 0, sizeof(*lib));
}

int wb_media_add(wb_media_lib *lib, const char *path, const char *name, int type) {
    if (!lib || lib->n_entries >= 256) return -1;
    int idx = lib->n_entries++;
    wb_media_entry *e = &lib->entries[idx];
    strncpy(e->path, path, 511);
    strncpy(e->name, name, 63);
    e->type = type;
    e->valid = 1;
    return idx;
}

/* Render graph */
void wb_graph_init(wb_render_graph *g) {
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->output_node = -1;
}

int wb_graph_add_media(wb_render_graph *g, const char *name, int media_index) {
    if (!g || g->n_nodes >= 128) return -1;
    int idx = g->n_nodes++;
    wb_render_node *n = &g->nodes[idx];
    n->type = RENDER_SRC_MEDIA;
    strncpy(n->name, name, 63);
    n->params.media_index = media_index;
    return idx;
}

int wb_graph_add_filter(wb_render_graph *g, const char *name,
                          const char *filter_name, const char *filter_args,
                          int *inputs, int n_inputs) {
    if (!g || g->n_nodes >= 128) return -1;
    int idx = g->n_nodes++;
    wb_render_node *n = &g->nodes[idx];
    n->type = RENDER_FILTER;
    strncpy(n->name, name, 63);
    strncpy(n->params.filter.filter_name, filter_name, 63);
    strncpy(n->params.filter.filter_args, filter_args, 255);
    n->n_inputs = n_inputs < 4 ? n_inputs : 4;
    for (int i = 0; i < n->n_inputs; i++) n->inputs[i] = inputs[i];
    return idx;
}

int wb_graph_add_composite(wb_render_graph *g, const char *name,
                             int blend_mode, int *inputs, int n_inputs) {
    if (!g || g->n_nodes >= 128) return -1;
    int idx = g->n_nodes++;
    wb_render_node *n = &g->nodes[idx];
    n->type = RENDER_COMPOSITE;
    strncpy(n->name, name, 63);
    n->params.composite.blend_mode = blend_mode;
    n->params.composite.n_layers = n_inputs;
    n->n_inputs = n_inputs < 4 ? n_inputs : 4;
    for (int i = 0; i < n->n_inputs; i++) n->inputs[i] = inputs[i];
    return idx;
}

int wb_graph_set_output(wb_render_graph *g, int node_idx, const char *path) {
    if (!g || node_idx < 0 || node_idx >= g->n_nodes) return -1;
    g->output_node = node_idx;
    g->nodes[node_idx].type = RENDER_OUTPUT;
    strncpy(g->nodes[node_idx].params.output.output_path, path, 511);
    return 0;
}

/* Project */
void wb_ytpmv_project_init(wb_ytpmv_project *proj) {
    if (!proj) return;
    memset(proj, 0, sizeof(*proj));
    wb_media_init(&proj->media);
    wb_comp_timeline_init(&proj->composition, 640, 480, 30, 10);
    wb_graph_init(&proj->graph);
    proj->output_quality = 23;
    proj->output_preset = 6;
    proj->ytpmv_bpm = 120;
    proj->ytpmv_scale = 2;
}

void wb_ytpmv_project_free(wb_ytpmv_project *proj) {
    if (!proj) return;
    wb_comp_timeline_free(&proj->composition);
}

int wb_project_build_ytpmv(wb_ytpmv_project *proj, const char *audio_path,
                             const char *video_path, float bpm) {
    if (!proj) return -1;
    
    int audio_idx = wb_media_add(&proj->media, audio_path, "source_audio", MEDIA_AUDIO);
    int video_idx = wb_media_add(&proj->media, video_path, "source_video", MEDIA_VIDEO);
    
    proj->ytpmv_bpm = bpm;
    proj->ytpmv_mode = 1;
    proj->composition.bpm = bpm;
    
    int audio_src = wb_graph_add_media(&proj->graph, "audio", audio_idx);
    int video_src = wb_graph_add_media(&proj->graph, "video", video_idx);
    
    int pitch_inputs[] = {audio_src};
    int pitch_node = wb_graph_add_filter(&proj->graph, "pitch_correct",
                                           "rubberband", "pitch=1.0", pitch_inputs, 1);
    
    int ck_inputs[] = {video_src};
    int ck_node = wb_graph_add_filter(&proj->graph, "chroma_key",
                                        "colorchannelmixer", "aa=0.5", ck_inputs, 1);
    
    int comp_inputs[] = {ck_node};
    int comp_node = wb_graph_add_composite(&proj->graph, "main_comp", 0, comp_inputs, 1);
    
    wb_graph_set_output(&proj->graph, comp_node, proj->output_path);
    
    return 0;
}
