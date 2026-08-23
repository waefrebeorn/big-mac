/* wb_export_job.c — G38 (R072): one-slot background render queue.
 * EXPORT spawns a pthread running wb_video_export_full on a DEEP COPY of the
 * session (the UI keeps editing the live one). Progress is published through
 * a volatile double the UI polls; CANCEL sets a volatile flag checked between
 * render chunks and during encode (ffmpeg child killed via SIGTERM). */
#include "wbus/wbus_export_job.h"
#include "wbus/wbus.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <stdio.h>

static void job_prog(void *ctx, double p) {
    wb_export_job *j = ctx;
    if (p > j->progress) j->progress = p;      /* monotonic */
}

static void *job_thread(void *arg) {
    wb_export_job *j = arg;
    j->running = 1;
    j->progress = 0.0;
    j->rc = wb_video_export_full(j->session, NULL,   /* export builds its own offline engine */
                                 j->output,
                                 j->srt[0] ? j->srt : NULL,
                                 (wb_video_codec)j->codec,
                                 j->range_start, j->range_dur,
                                 j->res_h,
                                 job_prog, j,
                                 &j->cancelled);
    if (j->rc == 0) j->progress = 1.0;
    else if (j->cancelled) j->progress = 0.0;
    j->done = 1;
    j->running = 0;
    return NULL;
}

int wb_export_job_start(wb_export_job *j, const struct wb_session *s,
                        const char *output_path, const char *srt_path,
                        int codec,
                        double range_start, double range_dur, int res_h) {
    if (!j || !s || !output_path) return -1;
    if (j->running) return -2;                 /* one-job queue: already busy */
    if (j->thread) wb_export_job_reset(j);     /* reap the previous worker */

    memset(j, 0, sizeof *j);
    snprintf(j->output, sizeof j->output, "%s", output_path);
    if (srt_path) snprintf(j->srt, sizeof j->srt, "%s", srt_path);
    j->codec = codec;
    j->range_start = range_start;
    j->range_dur = range_dur;
    j->res_h = res_h;
    j->session = wb_session_copy(s);
    if (!j->session) return -1;

    pthread_t *th = malloc(sizeof(pthread_t));
    if (!th) { wb_session_destroy(j->session); j->session = NULL; return -1; }
    if (pthread_create(th, NULL, job_thread, j) != 0) {
        free(th);
        wb_session_destroy(j->session); j->session = NULL;
        return -1;
    }
    j->thread = th;
    while (!j->running && !j->done) sched_yield();   /* `running` reliable on return */
    return 0;
}

void wb_export_job_cancel(wb_export_job *j) {
    if (j && j->running) j->cancelled = 1;
}

void wb_export_job_wait(wb_export_job *j) {
    if (!j || !j->thread) return;
    pthread_t *th = j->thread;
    pthread_join(*th, NULL);
    free(th);
    j->thread = NULL;
    wb_session_destroy(j->session);
    j->session = NULL;
}

void wb_export_job_reset(wb_export_job *j) {
    wb_export_job_wait(j);
    if (j) memset(j, 0, sizeof *j);
}

int wb_export_job_running(const wb_export_job *j) {
    return j ? j->running : 0;
}
