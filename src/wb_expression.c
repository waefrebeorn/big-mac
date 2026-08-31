/* src/wb_expression.c — expression maps for articulation management.
 * G91: Cubase-style per-note articulation switching for orchestral libraries.
 *
 * An expression map is a named collection of articulations. Each articulation
 * specifies a MIDI channel, optional CC number/value, and optional keyswitch
 * note. Applying an articulation to a note stamps the articulation index onto
 * the note (the engine reads this at render time to emit the appropriate
 * MIDI channel / CC / keyswitch events).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus.h"

/* ---- expression map CRUD ---------------------------------------------- */

int wb_session_add_expression_map(wb_session *s, const char *name) {
    if (!s || !name) return -1;
    if (s->expr_map_count >= WB_MAX_EXPRESSION_MAPS) return -1;
    int id = (int)s->expr_map_count;
    snprintf(s->expr_maps[id].name, sizeof(s->expr_maps[id].name), "%s", name);
    s->expr_maps[id].articulation_count = 0;
    s->expr_maps[id].active_articulation = -1;
    s->expr_map_count++;
    return id;
}

int wb_session_add_articulation(wb_session *s, int map_id, const char *name,
                                int channel, int cc, int value, int keyswitch) {
    if (!s || !name) return -1;
    if (map_id < 0 || map_id >= (int)s->expr_map_count) return -1;
    if (s->expr_maps[map_id].articulation_count >= WB_MAX_ARTICULATIONS_PER_MAP)
        return -1;
    int aid = (int)s->expr_maps[map_id].articulation_count;
    snprintf(s->expr_maps[map_id].articulations[aid].name,
             sizeof(s->expr_maps[map_id].articulations[aid].name), "%s", name);
    s->expr_maps[map_id].articulations[aid].midi_channel = channel;
    s->expr_maps[map_id].articulations[aid].cc_number = cc;
    s->expr_maps[map_id].articulations[aid].cc_value = value;
    s->expr_maps[map_id].articulations[aid].keyswitch = keyswitch;
    s->expr_maps[map_id].articulation_count++;
    return aid;
}

int wb_session_set_active_articulation(wb_session *s, int map_id, int articulation_idx) {
    if (!s) return -1;
    if (map_id < 0 || map_id >= (int)s->expr_map_count) return -1;
    if (articulation_idx < 0 ||
        articulation_idx >= (int)s->expr_maps[map_id].articulation_count)
        return -1;
    s->expr_maps[map_id].active_articulation = articulation_idx;
    return 0;
}

int wb_session_get_articulation_count(wb_session *s, int map_id) {
    if (!s) return -1;
    if (map_id < 0 || map_id >= (int)s->expr_map_count) return -1;
    return (int)s->expr_maps[map_id].articulation_count;
}

const char *wb_session_get_articulation_name(wb_session *s, int map_id, int idx) {
    if (!s) return "";
    if (map_id < 0 || map_id >= (int)s->expr_map_count) return "";
    if (idx < 0 || idx >= (int)s->expr_maps[map_id].articulation_count) return "";
    return s->expr_maps[map_id].articulations[idx].name;
}

/* ---- apply articulation to a note ------------------------------------- */

int wb_session_apply_articulation_to_note(wb_session *s, int track, int note_idx,
                                         int articulation_idx) {
    if (!s) return -1;
    if (track < 0 || track >= WB_MAX_TRACK_LANES) return -1;
    if (track >= (int)s->track_count) return -1;
    /* Look up the expression map assigned to this track's expression lane. */
    int map_id = s->track_expr_lane[track];
    if (map_id < 0 || map_id >= (int)s->expr_map_count) return -1;
    /* Validate articulation index against the map. */
    if (articulation_idx < 0 ||
        articulation_idx >= (int)s->expr_maps[map_id].articulation_count)
        return -1;
    wb_track *tr = &s->tracks[track];
    if (tr->clip_count == 0) return -1;
    /* Apply to the last clip (same clip wb_session_add_note targets). */
    wb_clip *cl = &tr->clips[tr->clip_count - 1];
    if (note_idx < 0 || note_idx >= (int)cl->note_count) return -1;
    cl->notes[note_idx].articulation = articulation_idx;
    return 0;
}

/* ---- expression lane (per-track map binding) -------------------------- */

int wb_session_set_expression_lane(wb_session *s, int track, int map_id) {
    if (!s) return -1;
    if (track < 0 || track >= WB_MAX_TRACK_LANES) return -1;
    if (map_id < -1 || map_id >= (int)s->expr_map_count) return -1;
    s->track_expr_lane[track] = map_id;
    return 0;
}

int wb_session_expression_map_count(wb_session *s) {
    if (!s) return -1;
    return (int)s->expr_map_count;
}