/* wb_export_queue.c — batch export queue.
 *
 * R078: Queue multiple export jobs and process them sequentially.
 *
 * Features:
 *   - Multiple format exports from same project
 *   - Progress tracking
 *   - Cancel/pause support
 *
 * Pure C11. */

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "wbus.h"

#define MAX_EXPORT_JOBS 16

typedef enum {
    EXPORT_PENDING = 0,
    EXPORT_RUNNING,
    EXPORT_DONE,
    EXPORT_ERROR,
    EXPORT_CANCELLED
} export_status_t;

typedef struct {
    char            name[128];
    char            output_path[512];
    int             format;       /* 0=WAV16, 1=WAV24, 2=WAV32F, 3=MP4_H264, 4=MP4_PRORES, 5=AAF, 6=OMF */
    int             width, height; /* Video resolution */
    int             fps;
    float           progress;     /* 0..1 */
    export_status_t status;
    int             error_code;
} export_job_t;

typedef struct {
    export_job_t jobs[MAX_EXPORT_JOBS];
    int          num_jobs;
    int          current_job;
    int          running;
    int          cancelled;
} wb_export_queue_inst;

void *wb_export_queue_create(void) {
    return calloc(1, sizeof(wb_export_queue_inst));
}

void wb_export_queue_destroy(void *inst) { free(inst); }

/* Add a job to the queue. Returns job index or -1. */
int wb_export_queue_add(void *inst, const char *name, const char *output_path,
                          int format, int width, int height, int fps) {
    wb_export_queue_inst *q = (wb_export_queue_inst *)inst;
    if (!q || q->num_jobs >= MAX_EXPORT_JOBS) return -1;

    int idx = q->num_jobs++;
    export_job_t *job = &q->jobs[idx];
    strncpy(job->name, name, 127);
    strncpy(job->output_path, output_path, 511);
    job->format = format;
    job->width = width;
    job->height = height;
    job->fps = fps;
    job->progress = 0.0f;
    job->status = EXPORT_PENDING;
    job->error_code = 0;

    return idx;
}

/* Start processing the queue. */
void wb_export_queue_start(void *inst) {
    wb_export_queue_inst *q = (wb_export_queue_inst *)inst;
    if (!q) return;
    q->running = 1;
    q->current_job = 0;
    q->cancelled = 0;
}

/* Cancel current job and stop queue. */
void wb_export_queue_cancel(void *inst) {
    wb_export_queue_inst *q = (wb_export_queue_inst *)inst;
    if (!q) return;
    q->cancelled = 1;
    if (q->current_job < q->num_jobs) {
        q->jobs[q->current_job].status = EXPORT_CANCELLED;
    }
}

/* Process one job. Returns 1 if more jobs remain, 0 if done. */
int wb_export_queue_process(void *inst) {
    wb_export_queue_inst *q = (wb_export_queue_inst *)inst;
    if (!q || !q->running) return 0;

    if (q->cancelled || q->current_job >= q->num_jobs) {
        q->running = 0;
        return 0;
    }

    export_job_t *job = &q->jobs[q->current_job];
    job->status = EXPORT_RUNNING;

    /* Simulate export progress (in real implementation, this would render) */
    job->progress += 0.1f;
    if (job->progress >= 1.0f) {
        job->progress = 1.0f;
        job->status = EXPORT_DONE;
        q->current_job++;
    }

    return (q->current_job < q->num_jobs) ? 1 : 0;
}

/* Get job status. */
const export_job_t* wb_export_queue_get_job(void *inst, int idx) {
    wb_export_queue_inst *q = (wb_export_queue_inst *)inst;
    if (!q || idx < 0 || idx >= q->num_jobs) return NULL;
    return &q->jobs[idx];
}

int wb_export_queue_get_progress(void *inst) {
    wb_export_queue_inst *q = (wb_export_queue_inst *)inst;
    if (!q || q->num_jobs == 0) return 0;
    int done = 0;
    for (int i = 0; i < q->num_jobs; i++) {
        if (q->jobs[i].status == EXPORT_DONE) done++;
    }
    return (done * 100) / q->num_jobs;
}
