/* wb_vca.c — VCA fader groups.
 *
 * R077: Master fader control over groups of channels.
 *
 * Algorithm:
 *   Each channel has a VCA membership bitmask
 *   VCA master fader multiplies member channel gains
 *   Does NOT affect post-fader sends (unlike subgroups)
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_VCA_GROUPS 16
#define MAX_CHANNELS 64

typedef struct {
    float    fader_db;       /* VCA master fader position */
    uint64_t members;        /* Bitmask of member channels */
    int      active;
} vca_group_t;

typedef struct {
    vca_group_t groups[MAX_VCA_GROUPS];
    int         num_groups;
    float       channel_base_gain[MAX_CHANNELS]; /* Per-channel base gain */
} wb_vca_inst;

void *wb_vca_create(void) {
    wb_vca_inst *vca = (wb_vca_inst *)calloc(1, sizeof(*vca));
    if (!vca) return NULL;
    /* Initialize all base gains to 1.0 (0 dB) */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        vca->channel_base_gain[i] = 1.0f;
    }
    return vca;
}

void wb_vca_destroy(void *inst) { free(inst); }

/* Create a new VCA group. Returns group index or -1. */
int wb_vca_create_group(void *inst) {
    wb_vca_inst *vca = (wb_vca_inst *)inst;
    if (!vca || vca->num_groups >= MAX_VCA_GROUPS) return -1;
    int idx = vca->num_groups++;
    vca->groups[idx].fader_db = 0.0f;
    vca->groups[idx].members = 0;
    vca->groups[idx].active = 1;
    return idx;
}

/* Assign a channel to a VCA group. */
void wb_vca_assign(void *inst, int group_idx, int channel_idx) {
    wb_vca_inst *vca = (wb_vca_inst *)inst;
    if (!vca || group_idx < 0 || group_idx >= vca->num_groups) return;
    if (channel_idx < 0 || channel_idx >= MAX_CHANNELS) return;
    vca->groups[group_idx].members |= (1ULL << channel_idx);
}

/* Remove a channel from a VCA group. */
void wb_vca_unassign(void *inst, int group_idx, int channel_idx) {
    wb_vca_inst *vca = (wb_vca_inst *)inst;
    if (!vca || group_idx < 0 || group_idx >= vca->num_groups) return;
    vca->groups[group_idx].members &= ~(1ULL << channel_idx);
}

/* Set VCA master fader. */
void wb_vca_set_fader(void *inst, int group_idx, float db) {
    wb_vca_inst *vca = (wb_vca_inst *)inst;
    if (!vca || group_idx < 0 || group_idx >= vca->num_groups) return;
    vca->groups[group_idx].fader_db = db;
}

/* Compute effective gain for a channel (product of all VCA masters it belongs to). */
float wb_vca_get_channel_gain(wb_vca_inst *vca, int channel_idx) {
    if (!vca || channel_idx < 0 || channel_idx >= MAX_CHANNELS) return 1.0f;

    float gain = vca->channel_base_gain[channel_idx];

    for (int g = 0; g < vca->num_groups; g++) {
        if (!vca->groups[g].active) continue;
        if (vca->groups[g].members & (1ULL << channel_idx)) {
            gain *= powf(10.0f, vca->groups[g].fader_db / 20.0f);
        }
    }

    return gain;
}
