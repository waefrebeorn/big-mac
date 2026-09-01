/* wb_multicam.c — multi-camera editing (R086).
 *
 * Vegas 10 feature: sync multiple camera angles and switch between
 * them "live" on the timeline.
 *
 * Implementation:
 *   - wb_multicam_group: a group of clips from different cameras
 *     synced to the same timeline position
 *   - The group appears as a single clip on the timeline
 *   - During playback/editing, the active angle can be switched
 *   - After editing, the group can be expanded back to separate tracks
 *
 * Pure C11.
 */

#include "wbus/wbus_edit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_ANGLES 16

typedef struct {
    char source_path[512];
    int track_idx;  /* which track this angle lives on */
    int clip_idx;   /* which clip on that track */
} mc_angle_t;

typedef struct {
    char name[128];
    mc_angle_t angles[MAX_ANGLES];
    int num_angles;
    int active_angle;  /* currently selected angle */
    double timeline_pos;
    double duration;
    int expanded;  /* 0 = single track, 1 = expanded to multiple tracks */
} mc_group_t;

#define MAX_GROUPS 32
static mc_group_t g_multicam_groups[MAX_GROUPS];
static int g_multicam_count = 0;

/* Create a multi-camera group from clips on different tracks at the same
 * timeline position. Returns group index or -1. */
int wb_multicam_create_group(wb_edit_graph *g, const char *name,
                              int *track_indices, int *clip_indices,
                              int num_angles, double timeline_pos) {
    if (!g || num_angles <= 0 || num_angles > MAX_ANGLES) return -1;
    if (g_multicam_count >= MAX_GROUPS) return -1;

    int idx = g_multicam_count++;
    mc_group_t *grp = &g_multicam_groups[idx];
    snprintf(grp->name, sizeof(grp->name), "%s", name);
    grp->num_angles = num_angles;
    grp->active_angle = 0;
    grp->timeline_pos = timeline_pos;
    grp->expanded = 0;

    for (int i = 0; i < num_angles; i++) {
        if (track_indices[i] < 0 || (uint32_t)track_indices[i] >= g->track_count) {
            g_multicam_count--;
            return -1;
        }
        wb_edit_track *tr = &g->tracks[track_indices[i]];
        if ((uint32_t)clip_indices[i] >= tr->clip_count) {
            g_multicam_count--;
            return -1;
        }
        wb_edit_clip *cl = &tr->clips[clip_indices[i]];
        strncpy(grp->angles[i].source_path, cl->source_path,
                sizeof(grp->angles[i].source_path) - 1);
        grp->angles[i].track_idx = track_indices[i];
        grp->angles[i].clip_idx = clip_indices[i];
        grp->duration = cl->duration;
    }

    return idx;
}

/* Switch the active angle in a group. */
int wb_multicam_set_active_angle(int group_idx, int angle) {
    if (group_idx < 0 || group_idx >= g_multicam_count) return -1;
    mc_group_t *grp = &g_multicam_groups[group_idx];
    if (angle < 0 || angle >= grp->num_angles) return -1;
    grp->active_angle = angle;
    return 0;
}

/* Get the active angle's source path. */
const char *wb_multicam_get_active_path(int group_idx) {
    if (group_idx < 0 || group_idx >= g_multicam_count) return NULL;
    mc_group_t *grp = &g_multicam_groups[group_idx];
    return grp->angles[grp->active_angle].source_path;
}

/* Expand a multicam group back to individual tracks. */
int wb_multicam_expand(int group_idx) {
    if (group_idx < 0 || group_idx >= g_multicam_count) return -1;
    mc_group_t *grp = &g_multicam_groups[group_idx];
    grp->expanded = 1;
    return 0;
}

/* Get group info. */
int wb_multicam_get_group(int group_idx, mc_group_t *out) {
    if (group_idx < 0 || group_idx >= g_multicam_count || !out) return -1;
    *out = g_multicam_groups[group_idx];
    return 0;
}

int wb_multicam_count(void) {
    return g_multicam_count;
}

void wb_multicam_clear(void) {
    g_multicam_count = 0;
}
