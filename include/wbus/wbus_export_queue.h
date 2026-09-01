/* wbus_export_queue.h — batch export queue API (R078/R085).
 *
 * Queues multiple export jobs and processes them sequentially.
 * Wired to the real node pipeline via wb_edit_graph_render_to_mp4().
 */

#ifndef WBUS_EXPORT_QUEUE_H
#define WBUS_EXPORT_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Job status codes */
typedef enum {
    EXPORT_PENDING = 0,
    EXPORT_RUNNING,
    EXPORT_DONE,
    EXPORT_ERROR,
    EXPORT_CANCELLED
} wb_export_status_t;

/* Export queue lifecycle */
void *wb_export_queue_create(void);
void  wb_export_queue_destroy(void *inst);

/* Add jobs */
int wb_export_queue_add(void *inst, const char *name, const char *output_path,
                         int format, int width, int height, int fps);
int wb_export_queue_add_edit_job(void *inst, const char *name,
                                  struct wb_edit_graph *g, const char *output_path);

/* Control */
void wb_export_queue_start(void *inst);
void wb_export_queue_cancel(void *inst);
int  wb_export_queue_process(void *inst);  /* returns 1 if more jobs remain */

/* Queries */
int          wb_export_queue_count(void *inst);
int          wb_export_queue_get_progress(void *inst);  /* 0..100 */
const char  *wb_export_queue_job_name(void *inst, int idx);
const char  *wb_export_queue_job_output(void *inst, int idx);
int          wb_export_queue_job_status(void *inst, int idx);  /* wb_export_status_t */
float        wb_export_queue_job_progress(void *inst, int idx);

#ifdef __cplusplus
}
#endif

#endif /* WBUS_EXPORT_QUEUE_H */
