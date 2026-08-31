/* wb_bg_render.c — background/offline rendering thread.
 * Starts a pthread that runs the same engine render path as wb_render.c,
 * reporting progress atomically and honoring a cancel flag. The UI calls
 * poll/wait/cancel from the main thread; the render runs async.
 *
 * Format: 0=WAV16, 1=WAV32F, 2=MP3 (via ffmpeg), 3=MP4 (video render).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "wbus.h"
#include "wb_internal.h"

/* ---- status codes ----------------------------------------------------- */
#define WB_BG_STATUS_PENDING   0
#define WB_BG_STATUS_RUNNING   1
#define WB_BG_STATUS_DONE      2
#define WB_BG_STATUS_ERROR     3
#define WB_BG_STATUS_CANCELLED 4

/* ---- format codes ----------------------------------------------------- */
#define WB_BG_FORMAT_WAV16  0
#define WB_BG_FORMAT_WAV32F 1
#define WB_BG_FORMAT_MP3    2
#define WB_BG_FORMAT_MP4    3

struct wb_bg_render {
    pthread_t thread;
    pthread_mutex_t lock;         /* protects status/progress/error_msg */
    /* parameters (immutable after start) */
    wb_session *session;          /* caller-owned; we reference only */
    char output_path[1024];
    int format;
    /* state (lock-protected) */
    int status;                   /* WB_BG_STATUS_* */
    float progress;               /* 0..1 */
    char error_msg[512];
    /* cancel flag — atomic, polled by the render loop */
    volatile int cancel;
};

/* ---- helpers ---------------------------------------------------------- */

static void bg_set_status(wb_bg_render *r, int status) {
    pthread_mutex_lock(&r->lock);
    r->status = status;
    pthread_mutex_unlock(&r->lock);
}

static void bg_set_error(wb_bg_render *r, const char *msg) {
    pthread_mutex_lock(&r->lock);
    r->status = WB_BG_STATUS_ERROR;
    snprintf(r->error_msg, sizeof(r->error_msg), "%s", msg);
    pthread_mutex_unlock(&r->lock);
}

static void bg_set_progress(wb_bg_render *r, float p) {
    pthread_mutex_lock(&r->lock);
    r->progress = p > 1.0f ? 1.0f : (p < 0.0f ? 0.0f : p);
    pthread_mutex_unlock(&r->lock);
}

/* Write the rendered PCM to the output file in the chosen format.
 * Returns 0 on success, -1 on error (error_msg set). */
static int bg_write_output(wb_bg_render *r, const wb_sample *pcm,
                           uint32_t frames, uint32_t sr) {
    if (r->format == WB_BG_FORMAT_WAV16) {
        return wb_wav_write_pcm16(r->output_path, pcm, frames, 2, sr);
    } else if (r->format == WB_BG_FORMAT_WAV32F) {
        return wb_wav_write_f32(r->output_path, pcm, frames, 2, sr);
    } else if (r->format == WB_BG_FORMAT_MP3) {
        /* render to temp WAV, then ffmpeg to MP3 */
        char tmpwav[1100];
        snprintf(tmpwav, sizeof(tmpwav), "%s.wav", r->output_path);
        if (wb_wav_write_pcm16(tmpwav, pcm, frames, 2, sr) != 0) return -1;
        char cmd[2048];
        snprintf(cmd, sizeof cmd,
            "ffmpeg -y -loglevel error -i '%s' -codec:a libmp3lame -q:a 2 '%s'",
            tmpwav, r->output_path);
        int rc = system(cmd);
        remove(tmpwav);
        return rc == 0 ? 0 : -1;
    } else if (r->format == WB_BG_FORMAT_MP4) {
        /* audio-only MP4 (no video). Uses the video export path with a
         * silent/black video track — here we just produce an AAC-in-mp4
         * from a temp WAV for simplicity. */
        char tmpwav[1100];
        snprintf(tmpwav, sizeof(tmpwav), "%s.wav", r->output_path);
        if (wb_wav_write_pcm16(tmpwav, pcm, frames, 2, sr) != 0) return -1;
        char cmd[2048];
        snprintf(cmd, sizeof cmd,
            "ffmpeg -y -loglevel error -f lavfi -i color=c=black:s=320x240:d=%f "
            "-i '%s' -c:v libx264 -tune stillimage -c:a aac -shortest -pix_fmt yuv420p '%s'",
            frames / (double)sr, tmpwav, r->output_path);
        int rc = system(cmd);
        remove(tmpwav);
        return rc == 0 ? 0 : -1;
    }
    return -1;
}

/* ---- render thread entry point --------------------------------------- */
static void *bg_render_thread(void *arg) {
    wb_bg_render *r = (wb_bg_render *)arg;
    wb_session *s = r->session;

    /* validate session */
    if (!s || s->length <= 0) {
        bg_set_error(r, "invalid session (null or zero length)");
        return NULL;
    }

    /* validate output path: directory must exist */
    {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", r->output_path);
        char *sl = strrchr(dir, '/');
        if (sl) {
            *sl = '\0';
            struct stat st;
            if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
                bg_set_error(r, "output directory does not exist");
                return NULL;
            }
        }
    }

    bg_set_status(r, WB_BG_STATUS_RUNNING);

    /* create a private engine for this render */
    wb_engine *tmp = wb_engine_create();
    if (!tmp) {
        bg_set_error(r, "engine creation failed");
        return NULL;
    }
    wb_engine_set_session(tmp, s);
    wb_engine_seek(tmp, 0);
    wb_engine_play(tmp);

    uint32_t total = (uint32_t)s->length;
    uint32_t sr = WB_SAMPLE_RATE;
    wb_sample *buf = malloc(total * 2 * sizeof(wb_sample));
    if (!buf) {
        wb_engine_destroy(tmp);
        bg_set_error(r, "render buffer allocation failed");
        return NULL;
    }

    /* chunked render loop with cancel + progress */
    uint32_t done = 0;
    while (done < total) {
        if (r->cancel) {
            free(buf);
            wb_engine_destroy(tmp);
            bg_set_status(r, WB_BG_STATUS_CANCELLED);
            return NULL;
        }
        uint32_t n = total - done;
        if (n > WB_MAX_BLOCK) n = WB_MAX_BLOCK;
        wb_engine_render(tmp, buf + done * 2, n);
        done += n;
        bg_set_progress(r, (float)done / (float)total);
    }
    wb_engine_destroy(tmp);

    /* write output in the chosen format */
    if (bg_write_output(r, buf, total, sr) != 0) {
        free(buf);
        bg_set_error(r, "output file write failed");
        return NULL;
    }
    free(buf);

    bg_set_progress(r, 1.0f);
    bg_set_status(r, WB_BG_STATUS_DONE);
    return NULL;
}

/* ---- public API ------------------------------------------------------- */

wb_bg_render *wb_bg_render_start(const wb_session *session,
                                 const char *output_path, int format) {
    if (!session || !output_path) return NULL;
    if (format < WB_BG_FORMAT_WAV16 || format > WB_BG_FORMAT_MP4) return NULL;

    wb_bg_render *r = calloc(1, sizeof(*r));
    if (!r) return NULL;

    r->session = (wb_session *)session;   /* we only read; caller owns */
    snprintf(r->output_path, sizeof(r->output_path), "%s", output_path);
    r->format = format;
    r->status = WB_BG_STATUS_PENDING;
    r->progress = 0.0f;
    r->cancel = 0;
    r->error_msg[0] = '\0';
    pthread_mutex_init(&r->lock, NULL);

    if (pthread_create(&r->thread, NULL, bg_render_thread, r) != 0) {
        pthread_mutex_destroy(&r->lock);
        free(r);
        return NULL;
    }
    return r;
}

int wb_bg_render_poll(wb_bg_render *r, float *progress_out) {
    if (!r) return 0;
    pthread_mutex_lock(&r->lock);
    int running = (r->status == WB_BG_STATUS_RUNNING ||
                   r->status == WB_BG_STATUS_PENDING);
    if (progress_out) *progress_out = r->progress;
    pthread_mutex_unlock(&r->lock);
    return running ? 1 : 0;
}

int wb_bg_render_wait(wb_bg_render *r, int timeout_ms) {
    if (!r) return -1;
    if (timeout_ms <= 0) {
        /* infinite wait */
        void *retval;
        int rc = pthread_join(r->thread, &retval);
        pthread_mutex_destroy(&r->lock);
        free(r);
        return rc == 0 ? 0 : -1;
    }
    /* timed wait: poll at 10ms intervals up to timeout */
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        pthread_mutex_lock(&r->lock);
        int done = (r->status != WB_BG_STATUS_RUNNING &&
                    r->status != WB_BG_STATUS_PENDING);
        pthread_mutex_unlock(&r->lock);
        if (done) {
            void *retval;
            pthread_join(r->thread, &retval);
            pthread_mutex_destroy(&r->lock);
            free(r);
            return 0;
        }
        usleep(10000);   /* 10ms */
        elapsed += 10;
    }
    return 1;   /* timeout */
}

void wb_bg_render_cancel(wb_bg_render *r) {
    if (!r) return;
    r->cancel = 1;
}

int wb_bg_render_status(wb_bg_render *r) {
    if (!r) return -1;
    pthread_mutex_lock(&r->lock);
    int s = r->status;
    pthread_mutex_unlock(&r->lock);
    /* map internal status to public values (already identical) */
    switch (s) {
        case WB_BG_STATUS_PENDING:   return 0;
        case WB_BG_STATUS_RUNNING:   return 1;
        case WB_BG_STATUS_DONE:      return 2;
        case WB_BG_STATUS_ERROR:     return 3;
        case WB_BG_STATUS_CANCELLED: return 4;
        default: return -1;
    }
}

const char *wb_bg_render_error(wb_bg_render *r) {
    if (!r) return NULL;
    pthread_mutex_lock(&r->lock);
    const char *e = (r->status == WB_BG_STATUS_ERROR) ? r->error_msg : NULL;
    pthread_mutex_unlock(&r->lock);
    return e;
}

/* ---- cleanup (for non-wait teardown) --------------------------------- */
void wb_bg_render_destroy(wb_bg_render *r) {
    if (!r) return;
    r->cancel = 1;
    void *retval;
    pthread_join(r->thread, &retval);
    pthread_mutex_destroy(&r->lock);
    free(r);
}