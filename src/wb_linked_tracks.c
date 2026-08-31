/* wb_linked_tracks.c — Ableton-style linked track group editing.
 * A link group is a collection of track indices whose clips at the same
 * index share edit operations: move, trim, delete, and add-note apply
 * to all linked tracks simultaneously.
 */

#include <stdlib.h>
#include <string.h>
#include "wbus.h"

/* ---- internal helpers --------------------------------------------------- */

/* Validate group_id, return pointer to the session or NULL. */
static wb_session *validate_group(const wb_session *s, int group_id);

/* Find a track index in a link group. Returns index within group or -1. */
static int find_track_in_group(const wb_session *s, int group_id, int track_index) {
    for (int i = 0; i < s->link_groups[group_id].track_count; i++) {
        if (s->link_groups[group_id].track_list[i] == track_index)
            return i;
    }
    return -1;
}

/* ---- group lifecycle ---------------------------------------------------- */

int wb_session_create_link_group(wb_session *s, const int *track_indices, int num_tracks) {
    if (!s || !track_indices || num_tracks <= 0) return -1;
    if (s->link_group_count >= WB_MAX_LINK_GROUPS) return -1;
    /* Validate all track indices */
    for (int i = 0; i < num_tracks; i++) {
        if (track_indices[i] < 0 || (uint32_t)track_indices[i] >= s->track_count)
            return -1;
    }
    int gid = (int)s->link_group_count;
    s->link_groups[gid].track_count = num_tracks;
    memcpy(s->link_groups[gid].track_list, track_indices, num_tracks * sizeof(int));
    s->link_group_count++;
    return gid;
}

int wb_session_remove_link_group(wb_session *s, int group_id) {
    if (!validate_group(s, group_id)) return -1;
    /* Shift later groups down */
    for (int i = group_id; i + 1 < (int)s->link_group_count; i++) {
        s->link_groups[i] = s->link_groups[i + 1];
    }
    s->link_group_count--;
    /* Clear the now-unused slot */
    memset(&s->link_groups[s->link_group_count], 0, sizeof(s->link_groups[0]));
    return 0;
}

int wb_session_add_to_link_group(wb_session *s, int group_id, int track_index) {
    if (!validate_group(s, group_id)) return -1;
    if (track_index < 0 || (uint32_t)track_index >= s->track_count) return -1;
    if (s->link_groups[group_id].track_count >= WB_MAX_TRACKS) return -1;
    /* Don't add if already present */
    if (find_track_in_group(s, group_id, track_index) >= 0) return 0;
    s->link_groups[group_id].track_list[s->link_groups[group_id].track_count++] = track_index;
    return 0;
}

int wb_session_remove_from_link_group(wb_session *s, int group_id, int track_index) {
    if (!validate_group(s, group_id)) return -1;
    int pos = find_track_in_group(s, group_id, track_index);
    if (pos < 0) return -1; /* not in group */
    /* Shift down */
    for (int i = pos; i + 1 < s->link_groups[group_id].track_count; i++)
        s->link_groups[group_id].track_list[i] = s->link_groups[group_id].track_list[i + 1];
    s->link_groups[group_id].track_count--;
    return 0;
}

/* ---- linked editing operations ------------------------------------------ */

int wb_session_linked_move_clip(wb_session *s, int group_id, int track, int clip, double new_start) {
    (void)track; /* track param is for API symmetry; we apply to all group members */
    if (!validate_group(s, group_id)) return -1;
    if (new_start < 0) new_start = 0;
    /* Apply to all linked tracks that have a clip at index `clip` */
    for (int i = 0; i < s->link_groups[group_id].track_count; i++) {
        int ti = s->link_groups[group_id].track_list[i];
        if (ti < 0 || (uint32_t)ti >= s->track_count) continue;
        wb_track *tr = &s->tracks[ti];
        if ((uint32_t)clip >= tr->clip_count) continue;
        tr->clips[clip].start = new_start;
    }
    return 0;
}

int wb_session_linked_trim_clip(wb_session *s, int group_id, int track, int clip, double delta_head, double delta_tail) {
    (void)track; /* track param is for API symmetry; we apply to all group members */
    if (!validate_group(s, group_id)) return -1;
    for (int i = 0; i < s->link_groups[group_id].track_count; i++) {
        int ti = s->link_groups[group_id].track_list[i];
        if (ti < 0 || (uint32_t)ti >= s->track_count) continue;
        wb_track *tr = &s->tracks[ti];
        if ((uint32_t)clip >= tr->clip_count) continue;
        wb_clip *cl = &tr->clips[clip];
        /* Trim head: move start forward by delta_head, reduce length */
        double new_start = cl->start + delta_head;
        double new_length = cl->length - delta_head - delta_tail;
        if (new_start < 0) new_start = 0;
        if (new_length <= 0) new_length = 1; /* minimum 1 sample */
        cl->start = new_start;
        cl->length = new_length;
        /* For MIDI clips, clamp notes into the new range */
        if (cl->type == 0) {
            double clip_end = new_start + new_length;
            uint32_t w = 0;
            for (uint32_t n = 0; n < cl->note_count; n++) {
                wb_note *nt = &cl->notes[n];
                double ns = new_start + nt->start;
                double ne = ns + nt->dur;
                if (ns < new_start) { nt->start = 0; nt->dur -= (new_start - ns); }
                if (ne > clip_end) { nt->dur = clip_end - (new_start + nt->start); }
                if (nt->dur > 0) cl->notes[w++] = *nt;
            }
            cl->note_count = w;
        }
    }
    return 0;
}

int wb_session_linked_delete_clip(wb_session *s, int group_id, int track, int clip) {
    (void)track;
    if (!validate_group(s, group_id)) return -1;
    for (int i = 0; i < s->link_groups[group_id].track_count; i++) {
        int ti = s->link_groups[group_id].track_list[i];
        if (ti < 0 || (uint32_t)ti >= s->track_count) continue;
        wb_track *tr = &s->tracks[ti];
        if ((uint32_t)clip >= tr->clip_count) continue;
        /* Free the clip's data */
        free(tr->clips[clip].notes);
        free(tr->clips[clip].audio_data);
        /* Shift remaining clips down */
        for (uint32_t c = (uint32_t)clip; c + 1 < tr->clip_count; c++)
            tr->clips[c] = tr->clips[c + 1];
        tr->clip_count--;
    }
    return 0;
}

int wb_session_linked_add_note(wb_session *s, int group_id, int track, double start, double dur, int pitch, int vel) {
    (void)track;
    if (!validate_group(s, group_id)) return -1;
    for (int i = 0; i < s->link_groups[group_id].track_count; i++) {
        int ti = s->link_groups[group_id].track_list[i];
        if (ti < 0 || (uint32_t)ti >= s->track_count) continue;
        wb_track *tr = &s->tracks[ti];
        /* Add note to the first clip (same as wb_session_add_note behavior) */
        if (tr->clip_count == 0) {
            tr->clips = calloc(1, sizeof(wb_clip));
            if (!tr->clips) continue;
            tr->clip_count = 1;
            tr->clips[0].type = 0;
            tr->clips[0].start = 0;
        }
        wb_clip *cl = &tr->clips[0];
        wb_note *n = realloc(cl->notes, (cl->note_count + 1) * sizeof(wb_note));
        if (!n) continue;
        cl->notes = n;
        wb_note *nn = &cl->notes[cl->note_count++];
        nn->start = start;
        nn->dur = dur;
        nn->pitch = (uint8_t)pitch;
        nn->vel = (uint8_t)vel;
    }
    return 0;
}

/* ---- query -------------------------------------------------------------- */

int wb_session_get_link_group(const wb_session *s, int group_id, int *track_indices_out, int max_count) {
    if (!validate_group(s, group_id) || !track_indices_out || max_count <= 0) return -1;
    int n = s->link_groups[group_id].track_count;
    if (n > max_count) n = max_count;
    memcpy(track_indices_out, s->link_groups[group_id].track_list, n * sizeof(int));
    return n;
}

int wb_session_link_group_count(const wb_session *s) {
    if (!s) return 0;
    return (int)s->link_group_count;
}

/* Definition of validate_group (validates group_id is in range). */
static wb_session *validate_group(const wb_session *s, int group_id) {
    if (!s || group_id < 0 || group_id >= (int)s->link_group_count) return NULL;
    return (wb_session *)s;
}