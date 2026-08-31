/* wb_track_folder.c — track folder hierarchy and advanced bus routing.
 *
 * Folders group tracks in the arrangement view. A folder is itself a track
 * (kind == WB_TRACK_KIND_FOLDER) with metadata in the session's parallel
 * folders[] array tracking parent, collapsed state, and child membership.
 *
 * Bus routing uses the existing wb_track.route field (track -> bus/master)
 * and the send_level/send_target/send_pre aux-send slots.
 *
 * When a folder is muted/soloed, all child tracks inherit that state.
 */

#include <stdlib.h>
#include <string.h>
#include "wbus.h"

/* ---- helpers ------------------------------------------------------------ */

/* Find the folder-metadata slot index for a given folder track index.
 * The folder track's own index is its slot index (1:1 mapping). */
static int folder_slot(wb_session *s, int folder_idx) {
    if (!s || folder_idx < 0 || (uint32_t)folder_idx >= s->track_count) return -1;
    if (s->tracks[folder_idx].kind != WB_TRACK_KIND_FOLDER) return -1;
    return folder_idx;
}

/* Propagate folder mute state to all child tracks. */
static void folder_propagate_mute(wb_session *s, int folder_idx) {
    if (!s || folder_idx < 0) return;
    if ((uint32_t)folder_idx >= s->folder_count) return;
    wb_track *folder = &s->tracks[folder_idx];
    int mute = folder->mute;
    for (int i = 0; i < s->folders[folder_idx].track_count; i++) {
        int ti = s->folders[folder_idx].track_list[i];
        if (ti >= 0 && (uint32_t)ti < s->track_count) {
            s->tracks[ti].mute = mute;
        }
    }
}

/* Propagate folder solo state to all child tracks. */
static void folder_propagate_solo(wb_session *s, int folder_idx) {
    if (!s || folder_idx < 0) return;
    if ((uint32_t)folder_idx >= s->folder_count) return;
    wb_track *folder = &s->tracks[folder_idx];
    int solo = folder->solo;
    for (int i = 0; i < s->folders[folder_idx].track_count; i++) {
        int ti = s->folders[folder_idx].track_list[i];
        if (ti >= 0 && (uint32_t)ti < s->track_count) {
            s->tracks[ti].solo = solo;
        }
    }
}

/* ---- folder API -------------------------------------------------------- */

int wb_session_create_folder(wb_session *s, const char *name, int parent_folder_idx) {
    if (!s) return -1;
    if (s->track_count >= WB_MAX_TRACKS) return -1;
    if (s->folder_count >= WB_MAX_FOLDERS) return -1;
    /* Validate parent: -1 for top-level, or must be an existing folder track */
    if (parent_folder_idx >= 0) {
        if ((uint32_t)parent_folder_idx >= s->track_count) return -1;
        if (s->tracks[parent_folder_idx].kind != WB_TRACK_KIND_FOLDER) return -1;
    }

    /* Add the folder track */
    wb_track *tr = wb_session_add_track(s, name, WB_TRACK_KIND_FOLDER);
    if (!tr) return -1;
    int idx = (int)s->track_count - 1;

    /* Initialize folder metadata */
    uint32_t slot = s->folder_count++;
    s->folders[slot].parent_folder_idx = parent_folder_idx;
    s->folders[slot].collapsed = 0;
    s->folders[slot].track_count = 0;
    memset(s->folders[slot].track_list, 0, sizeof(s->folders[slot].track_list));

    /* Track's folder_idx points to itself (it IS the folder) */
    tr->folder_idx = idx;
    tr->route = -1;  /* folders route to master by default */

    return idx;
}

int wb_session_add_track_to_folder(wb_session *s, int track_idx, int folder_idx) {
    if (!s) return -1;
    if (track_idx < 0 || (uint32_t)track_idx >= s->track_count) return -1;
    int slot = folder_slot(s, folder_idx);
    if (slot < 0) return -1;
    /* Cannot add a folder track to another folder (flat folder nesting only) */
    if (s->tracks[track_idx].kind == WB_TRACK_KIND_FOLDER) return -1;
    /* Cannot add a track to itself */
    if (track_idx == folder_idx) return -1;

    /* Remove from previous folder if any */
    int prev = s->tracks[track_idx].folder_idx;
    if (prev >= 0 && prev != track_idx && (uint32_t)prev < s->folder_count) {
        wb_session_remove_track_from_folder(s, track_idx, prev);
    }

    /* Add to folder's track list */
    if (s->folders[slot].track_count >= WB_MAX_TRACKS) return -1;
    s->folders[slot].track_list[s->folders[slot].track_count++] = track_idx;
    s->tracks[track_idx].folder_idx = folder_idx;

    /* Inherit folder mute/solo state */
    if (s->tracks[folder_idx].mute) s->tracks[track_idx].mute = 1;
    if (s->tracks[folder_idx].solo) s->tracks[track_idx].solo = 1;

    return 0;
}

int wb_session_remove_track_from_folder(wb_session *s, int track_idx, int folder_idx) {
    if (!s) return -1;
    if (track_idx < 0 || (uint32_t)track_idx >= s->track_count) return -1;
    int slot = folder_slot(s, folder_idx);
    if (slot < 0) return -1;

    /* Find and remove from track_list */
    for (int i = 0; i < s->folders[slot].track_count; i++) {
        if (s->folders[slot].track_list[i] == track_idx) {
            /* Shift remaining entries */
            for (int j = i; j + 1 < s->folders[slot].track_count; j++)
                s->folders[slot].track_list[j] = s->folders[slot].track_list[j + 1];
            s->folders[slot].track_count--;
            s->tracks[track_idx].folder_idx = -1;
            return 0;
        }
    }
    return -1;  /* track not found in this folder */
}

int wb_session_set_folder_collapsed(wb_session *s, int folder_idx, int collapsed) {
    if (!s) return -1;
    int slot = folder_slot(s, folder_idx);
    if (slot < 0) return -1;
    s->folders[slot].collapsed = collapsed ? 1 : 0;
    return 0;
}

int wb_session_set_folder_mute(wb_session *s, int folder_idx, int mute) {
    if (!s) return -1;
    int slot = folder_slot(s, folder_idx);
    if (slot < 0) return -1;
    s->tracks[folder_idx].mute = mute ? 1 : 0;
    folder_propagate_mute(s, folder_idx);
    return 0;
}

int wb_session_set_folder_solo(wb_session *s, int folder_idx, int solo) {
    if (!s) return -1;
    int slot = folder_slot(s, folder_idx);
    if (slot < 0) return -1;
    s->tracks[folder_idx].solo = solo ? 1 : 0;
    folder_propagate_solo(s, folder_idx);
    return 0;
}

int wb_session_get_folder_track_count(wb_session *s, int folder_idx) {
    if (!s) return -1;
    int slot = folder_slot(s, folder_idx);
    if (slot < 0) return -1;
    return s->folders[slot].track_count;
}

int wb_session_get_folder_tracks(wb_session *s, int folder_idx, int *track_indices, int max_count) {
    if (!s || !track_indices || max_count <= 0) return -1;
    int slot = folder_slot(s, folder_idx);
    if (slot < 0) return -1;
    int n = s->folders[slot].track_count;
    if (n > max_count) n = max_count;
    memcpy(track_indices, s->folders[slot].track_list, n * sizeof(int));
    return n;
}

int wb_session_remove_folder(wb_session *s, int folder_idx) {
    if (!s) return -1;
    int slot = folder_slot(s, folder_idx);
    if (slot < 0) return -1;

    int parent = s->folders[slot].parent_folder_idx;

    /* Move children to parent folder (or top-level if folder was top-level) */
    for (int i = 0; i < s->folders[slot].track_count; i++) {
        int ti = s->folders[slot].track_list[i];
        if (ti >= 0 && (uint32_t)ti < s->track_count) {
            s->tracks[ti].folder_idx = parent;
            if (parent >= 0 && (uint32_t)parent < s->folder_count) {
                /* Add to parent's track list */
                if (s->folders[parent].track_count < WB_MAX_TRACKS) {
                    s->folders[parent].track_list[s->folders[parent].track_count++] = ti;
                }
            }
        }
    }

    /* Remove the folder track itself */
    wb_session_remove_track(s, (uint32_t)folder_idx);

    /* Note: wb_session_remove_track shifts track indices. The folders[] array
     * is indexed by track index, so after removal all folder slots above
     * folder_idx are now stale. We handle this by clearing the slot and
     * decrementing folder_count only if it was the last slot. For simplicity
     * and correctness with the 1:1 mapping, we mark the slot as empty. */
    memset(&s->folders[slot], 0, sizeof(s->folders[slot]));
    s->folders[slot].parent_folder_idx = -1;

    /* If this was the last folder slot, shrink the count */
    while (s->folder_count > 0) {
        uint32_t last = s->folder_count - 1;
        if (last < s->track_count && s->tracks[last].kind == WB_TRACK_KIND_FOLDER
            && s->folders[last].track_count >= 0) {
            /* Check if slot is actually still valid (has a name or children) */
            if (s->tracks[last].name[0] != '\0') break;
        }
        s->folder_count--;
        if (last == (uint32_t)slot) break;
        /* If the removed slot wasn't the last, we keep the count but slot is empty */
        break;
    }

    return 0;
}

/* ---- bus routing API ---------------------------------------------------- */

int wb_session_create_bus(wb_session *s, const char *name) {
    if (!s) return -1;
    wb_track *tr = wb_session_add_track(s, name, WB_TRACK_KIND_BUS);
    if (!tr) return -1;
    return (int)s->track_count - 1;
}

int wb_session_route_track_to(wb_session *s, int track_idx, int dest_idx) {
    if (!s) return -1;
    if (track_idx < 0 || (uint32_t)track_idx >= s->track_count) return -1;
    /* dest_idx=-1 routes to master; otherwise must be a valid bus track */
    if (dest_idx >= 0) {
        if ((uint32_t)dest_idx >= s->track_count) return -1;
        if (s->tracks[dest_idx].kind != WB_TRACK_KIND_BUS) return -1;
        /* Prevent self-routing */
        if (dest_idx == track_idx) return -1;
    }
    /* Don't allow routing a bus to itself */
    if (s->tracks[track_idx].kind == WB_TRACK_KIND_BUS && dest_idx == track_idx) return -1;
    s->tracks[track_idx].route = dest_idx;
    return 0;
}

int wb_session_set_send(wb_session *s, int src_track, int dest_track, float level, int send_index) {
    if (!s) return -1;
    if (src_track < 0 || (uint32_t)src_track >= s->track_count) return -1;
    if (send_index < 0 || send_index > 1) return -1;
    if (level < 0.0f) level = 0.0f;
    /* dest_track=-1 clears the send; otherwise must be a valid bus */
    if (dest_track >= 0) {
        if ((uint32_t)dest_track >= s->track_count) return -1;
        if (s->tracks[dest_track].kind != WB_TRACK_KIND_BUS) return -1;
    }
    wb_track *tr = &s->tracks[src_track];
    tr->send_target[send_index] = dest_track;
    tr->send_level[send_index] = level;
    return 0;
}

int wb_session_set_send_pre_fader(wb_session *s, int src_track, int send_index, int pre) {
    if (!s) return -1;
    if (src_track < 0 || (uint32_t)src_track >= s->track_count) return -1;
    if (send_index < 0 || send_index > 1) return -1;
    s->tracks[src_track].send_pre[send_index] = pre ? 1 : 0;
    return 0;
}