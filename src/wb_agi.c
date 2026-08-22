/* wb_agi.c — AGI control-surface bridge model (R043-G7).
 * Task/orchestration state. Pure C11, stdlib only. See wbus_agi.h. */

#include "wbus/wbus_agi.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define WB_AGI_MAX_TASKS 16
#define WB_AGI_LABEL_LEN 48

struct wb_agi {
    char          label[WB_AGI_MAX_TASKS][WB_AGI_LABEL_LEN];
    wb_agi_status status[WB_AGI_MAX_TASKS];
    float         progress[WB_AGI_MAX_TASKS];
    int           count;
    double        clock;
    char          last_event[96];
};

wb_agi *wb_agi_create(void) {
    return calloc(1, sizeof(wb_agi));
}

void wb_agi_destroy(wb_agi *a) { free(a); }

int wb_agi_submit(wb_agi *a, const char *label) {
    if (!a || !label || !label[0]) return -1;
    if (a->count >= WB_AGI_MAX_TASKS) return -1;
    int id = a->count++;
    snprintf(a->label[id], WB_AGI_LABEL_LEN, "%s", label);
    a->status[id]   = WB_AGI_QUEUED;
    a->progress[id] = 0.0f;
    snprintf(a->last_event, sizeof(a->last_event), "queued: %s", label);
    return id;
}

void wb_agi_tick(wb_agi *a, double dt) {
    if (!a) return;
    a->clock += dt;
    /* promote the first queued task to running */
    for (int i = 0; i < a->count; i++) {
        if (a->status[i] == WB_AGI_QUEUED) {
            a->status[i] = WB_AGI_RUNNING;
            snprintf(a->last_event, sizeof(a->last_event), "running: %s", a->label[i]);
            break;
        }
    }
    /* advance the running task(s): ~4s per task */
    for (int i = 0; i < a->count; i++) {
        if (a->status[i] != WB_AGI_RUNNING) continue;
        a->progress[i] += (float)(dt / 4.0);
        if (a->progress[i] >= 1.0f) {
            a->progress[i] = 1.0f;
            a->status[i] = WB_AGI_DONE;
            snprintf(a->last_event, sizeof(a->last_event), "done: %s", a->label[i]);
        }
    }
}

int wb_agi_task_count(const wb_agi *a) { return a ? a->count : 0; }

wb_agi_status wb_agi_task_status(const wb_agi *a, int id) {
    if (!a || id < 0 || id >= a->count) return WB_AGI_QUEUED;
    return a->status[id];
}

float wb_agi_task_progress(const wb_agi *a, int id) {
    if (!a || id < 0 || id >= a->count) return 0.0f;
    return a->progress[id];
}

const char *wb_agi_task_label(const wb_agi *a, int id) {
    if (!a || id < 0 || id >= a->count) return "";
    return a->label[id];
}

int wb_agi_running_count(const wb_agi *a) {
    if (!a) return 0;
    int n = 0;
    for (int i = 0; i < a->count; i++) if (a->status[i] == WB_AGI_RUNNING) n++;
    return n;
}

int wb_agi_done_count(const wb_agi *a) {
    if (!a) return 0;
    int n = 0;
    for (int i = 0; i < a->count; i++) if (a->status[i] == WB_AGI_DONE) n++;
    return n;
}

const char *wb_agi_last_event(const wb_agi *a) {
    return a ? a->last_event : "";
}
