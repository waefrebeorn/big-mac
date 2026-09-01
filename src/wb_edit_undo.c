/* wb_edit_undo.c — undo/redo for edit graph operations (R086).
 *
 * Snapshots the edit graph state before structural changes
 * (add/remove/move/split clip, add effect, etc.) and allows
 * undo/redo. Uses the same pattern as wb_undo.c but for
 * wb_edit_graph instead of wb_session.
 *
 * Pure C11.
 */

#include "wbus/wbus_edit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_UNDO 50

typedef struct {
    wb_edit_graph **states;  /* array of deep-copied graphs */
    int count;
    int current;  /* index of current state */
    int cap;
} wb_edit_undo_inst;

static wb_edit_undo_inst *g_edit_undo = NULL;

void wb_edit_undo_init(void) {
    if (g_edit_undo) return;
    g_edit_undo = calloc(1, sizeof(*g_edit_undo));
    g_edit_undo->cap = MAX_UNDO;
    g_edit_undo->states = calloc(g_edit_undo->cap, sizeof(wb_edit_graph*));
    g_edit_undo->count = 0;
    g_edit_undo->current = -1;
}

void wb_edit_undo_shutdown(void) {
    if (!g_edit_undo) return;
    for (int i = 0; i < g_edit_undo->count; i++) {
        if (g_edit_undo->states[i]) {
            wb_edit_graph_destroy(g_edit_undo->states[i]);
        }
    }
    free(g_edit_undo->states);
    free(g_edit_undo);
    g_edit_undo = NULL;
}

/* Deep copy an edit graph for snapshot */
static wb_edit_graph *edit_graph_copy(const wb_edit_graph *g) {
    if (!g) return NULL;
    wb_edit_graph *copy = wb_edit_graph_create(g->fps, g->width, g->height);
    if (!copy) return NULL;

    /* Copy settings */
    copy->duration = g->duration;
    copy->color_management_enabled = g->color_management_enabled;
    copy->input_cs = g->input_cs;
    copy->output_cs = g->output_cs;
    copy->tonemap = g->tonemap;
    copy->proxy_enabled = g->proxy_enabled;
    copy->proxy_w = g->proxy_w;
    copy->proxy_h = g->proxy_h;
    copy->eval_time = -1.0;

    /* Copy subtitle settings */
    memcpy(copy->subtitle_text, g->subtitle_text, sizeof(copy->subtitle_text));
    copy->subtitle_pos_x = g->subtitle_pos_x;
    copy->subtitle_pos_y = g->subtitle_pos_y;
    copy->subtitle_size = g->subtitle_size;
    copy->subtitle_color = g->subtitle_color;

    /* Copy tracks */
    for (uint32_t t = 0; t < g->track_count; t++) {
        if (t >= copy->track_count) wb_edit_add_track(copy, g->tracks[t].name);
        wb_edit_track *dst = &copy->tracks[t];
        wb_edit_track *src = &g->tracks[t];
        dst->volume = src->volume;
        dst->muted = src->muted;
        dst->soloed = src->soloed;

        /* Copy video clips */
        for (uint32_t c = 0; c < src->clip_count; c++) {
            wb_edit_add_clip(copy, (int)t, src->clips[c].source_path,
                             src->clips[c].start_in_source,
                             src->clips[c].duration,
                             src->clips[c].timeline_pos);
            dst->clips[c].speed = src->clips[c].speed;
            dst->clips[c].gain = src->clips[c].gain;
        }

        /* Copy audio clips */
        for (uint32_t a = 0; a < src->audio_clip_count; a++) {
            wb_edit_add_audio_clip(copy, (int)t,
                                   src->audio_clips[a].source_path,
                                   src->audio_clips[a].start_in_source,
                                   src->audio_clips[a].duration,
                                   src->audio_clips[a].timeline_pos);
            dst->audio_clips[a].volume = src->audio_clips[a].volume;
            dst->audio_clips[a].speed = src->audio_clips[a].speed;
        }

        /* Copy transitions */
        for (uint32_t tr = 0; tr < src->trans_count; tr++) {
            wb_edit_add_transition(copy, (int)t,
                                   src->transitions[tr].clip_a_idx,
                                   src->transitions[tr].type,
                                   src->transitions[tr].duration);
        }
    }

    return copy;
}

/* Save a snapshot before a structural change */
void wb_edit_undo_checkpoint(void) {
    if (!g_edit_undo) wb_edit_undo_init();

    /* Discard any redo states */
    for (int i = g_edit_undo->current + 1; i < g_edit_undo->count; i++) {
        if (g_edit_undo->states[i]) {
            wb_edit_graph_destroy(g_edit_undo->states[i]);
            g_edit_undo->states[i] = NULL;
        }
    }
    g_edit_undo->count = g_edit_undo->current + 1;

    /* Add new state (copy of current graph from agent) */
    if (g_edit_undo->count >= g_edit_undo->cap) {
        /* Shift oldest state out */
        if (g_edit_undo->states[0]) {
            wb_edit_graph_destroy(g_edit_undo->states[0]);
        }
        memmove(g_edit_undo->states, g_edit_undo->states + 1,
                (g_edit_undo->cap - 1) * sizeof(wb_edit_graph*));
        g_edit_undo->count--;
    }

    /* The caller must have set up the current graph pointer */
    /* We store NULL as a placeholder — the agent calls wb_edit_undo_set_current() */
    g_edit_undo->states[g_edit_undo->count] = NULL;
    g_edit_undo->current = g_edit_undo->count;
    g_edit_undo->count++;
}

/* Set the current graph to snapshot (used by agent after checkpoint) */
void wb_edit_undo_set_current(wb_edit_graph *g) {
    if (!g_edit_undo || g_edit_undo->current < 0) return;
    if (g_edit_undo->states[g_edit_undo->current]) {
        wb_edit_graph_destroy(g_edit_undo->states[g_edit_undo->current]);
    }
    g_edit_undo->states[g_edit_undo->current] = edit_graph_copy(g);
}

/* Undo: return the previous state, or NULL if none */
wb_edit_graph *wb_edit_undo_undo(wb_edit_graph *current) {
    if (!g_edit_undo || g_edit_undo->current <= 0) return NULL;
    g_edit_undo->current--;
    return edit_graph_copy(g_edit_undo->states[g_edit_undo->current]);
}

/* Redo: return the next state, or NULL if none */
wb_edit_graph *wb_edit_undo_redo(wb_edit_graph *current) {
    if (!g_edit_undo || g_edit_undo->current >= g_edit_undo->count - 1) return NULL;
    g_edit_undo->current++;
    return edit_graph_copy(g_edit_undo->states[g_edit_undo->current]);
}

int wb_edit_undo_can_undo(void) {
    return g_edit_undo && g_edit_undo->current > 0;
}

int wb_edit_undo_can_redo(void) {
    return g_edit_undo && g_edit_undo->current < g_edit_undo->count - 1;
}
