#ifndef WBUS_EXPORT_JOB_H
#define WBUS_EXPORT_JOB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* G38 (R072) — one-slot background render queue. EXPORT spawns a pthread
 * running wb_video_export_full on a DEEP COPY of the session, so the UI keeps
 * editing while the render proceeds. Progress 0..1 is polled by the UI each
 * frame; CANCEL sets a flag checked between render chunks and during encode
 * (the ffmpeg child is killed with SIGTERM). */

typedef struct wb_export_job {
    /* readable from the UI thread */
    volatile int    running;     /* 1 while the worker thread lives */
    volatile int    done;        /* 1 when finished (check rc) */
    volatile int    cancelled;   /* set by wb_export_job_cancel */
    volatile double progress;    /* 0..1 */
    int             rc;          /* 0 ok, -1 error, -2 cancelled */
    double          range_start; /* G39: seconds, <0 = whole session */
    double          range_dur;   /* G39: <=0 = until end */
    int             res_h;       /* G40: 0/1080 native, 480/720 scaled */
    char            output[512];

    /* worker-private */
    void           *thread;
    struct wb_session *session;  /* owned deep copy */
    char            srt[512];
    int             codec;
} wb_export_job;

/* Start a background export of `s` (deep-copied). Returns 0 on success,
 * -2 if a job is already running (one-job queue), -1 on failure. */
int  wb_export_job_start(wb_export_job *j, const struct wb_session *s,
                         const char *output_path, const char *srt_path,
                         int codec,
                         double range_start, double range_dur, int res_h);

void wb_export_job_cancel(wb_export_job *j);
void wb_export_job_wait(wb_export_job *j);
void wb_export_job_reset(wb_export_job *j);
int  wb_export_job_running(const wb_export_job *j);

#ifdef __cplusplus
}
#endif
#endif
