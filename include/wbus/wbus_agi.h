/*
 * wbus_agi.h — AGI control-surface bridge model (R043-G7).
 *
 * The AGI workspace tier's engine: a self-contained task/orchestration
 * state model. The DAW (or an external agent via wb_agent) submits
 * tasks ("render episode", "polish voice", "auto-cut shorts"); each task
 * has a status (queued/running/done/failed) and a progress value. The UI
 * renders the task list + a live activity readout; the module knows
 * nothing about SDL or the rest of the app. Opaque struct, C11, stdlib.
 */
#ifndef WUBUS_WBUS_AGI_H
#define WUBUS_WBUS_AGI_H


#ifdef __cplusplus
extern "C" {
#endif
typedef struct wb_agi wb_agi;

typedef enum {
    WB_AGI_QUEUED = 0,
    WB_AGI_RUNNING,
    WB_AGI_DONE,
    WB_AGI_FAILED
} wb_agi_status;

wb_agi *wb_agi_create(void);
void    wb_agi_destroy(wb_agi *a);

/* Submit a task. Returns the task id (>=0), or -1 if full/invalid. */
int  wb_agi_submit(wb_agi *a, const char *label);

/* Advance the model: promotes queued->running, advances progress of the
 * running task by dt * speed. Call once per frame/tick. */
void wb_agi_tick(wb_agi *a, double dt);

/* Task accessors (id-validity checked; no internals leak). */
int             wb_agi_task_count(const wb_agi *a);
wb_agi_status   wb_agi_task_status(const wb_agi *a, int id);
float           wb_agi_task_progress(const wb_agi *a, int id); /* 0..1 */
const char     *wb_agi_task_label(const wb_agi *a, int id);

/* Aggregate readout for the UI header. */
int  wb_agi_running_count(const wb_agi *a);
int  wb_agi_done_count(const wb_agi *a);
/* Last human-readable event line ("task 3 done: render episode"). */
const char *wb_agi_last_event(const wb_agi *a);


#ifdef __cplusplus
}
#endif
#endif /* WUBUS_WBUS_AGI_H */
