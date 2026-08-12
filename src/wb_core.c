/* wb_core.c — Big Mac DAW engine core.
 * Owns transport, the DSP graph/mixer, and the pull-based render path.
 * Realtime thread: render() + command draining only. No locks, no allocs.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "wbus.h"
#include "wbus_cmd.h"
#include "wbus_plugin.h"
#include "wbus_dsp.h"
#include "wb_internal.h"

/* one per track at render time */
struct wb_track_runtime {
    int    active;
    float  volume, pan;
    int    mute, solo;
    int    kind;
    void  *voice;                    /* instrument instance (kind 0) */
    wb_sample *bufL, *bufR;          /* per-track block buffers */
    uint32_t   buf_cap;
};
typedef struct wb_track_runtime wb_track_runtime;

struct wb_engine {
    wb_session    *session;          /* caller-owned */
    wb_transport   t;
    wb_cmd_queue   queue;
    wb_track_runtime *rtracks;
    wb_sample *accL, *accR;          /* master accumulation scratch */
    uint32_t   acc_cap;
    float cpu_load;
};

/* drain UI commands (RT thread, once per block) */
static void engine_process_cmds(wb_engine *e) {
    wb_cmd c;
    while (wb_cmd_pop(&e->queue, &c)) {
        switch (c.type) {
        case WB_CMD_PLAY: e->t.playing = 1; break;
        case WB_CMD_STOP: e->t.playing = 0; break;
        case WB_CMD_SEEK: e->t.song_pos = c.f0; break;
        case WB_CMD_SET_BPM: e->t.bpm = c.f0; break;
        case WB_CMD_SET_TRACK_VOL:
            if (e->rtracks && c.i0 >= 0 && c.i0 < (int64_t)WB_MAX_TRACKS)
                e->rtracks[c.i0].volume = (float)c.f0;
            break;
        case WB_CMD_NOTE:
            if (e->rtracks && c.i0 >= 0 && c.i0 < (int64_t)WB_MAX_TRACKS)
                if (e->rtracks[c.i0].voice)
                    wb_synth_note(e->rtracks[c.i0].voice, (int)c.i1, (int)c.f0);
            break;
        default: break;
        }
    }
}

/* render one track's block into its private buffers */
static void render_track(wb_engine *e, int ti, uint32_t frames) {
    wb_track_runtime *tr = &e->rtracks[ti];
    memset(tr->bufL, 0, frames * sizeof(wb_sample));
    memset(tr->bufR, 0, frames * sizeof(wb_sample));

    if (tr->kind == 0 && tr->voice) {
        /* schedule notes from MIDI clips that fall in this block */
        wb_track *tk = &e->session->tracks[ti];
        wb_transport_schedule_notes(tk, e->t.song_pos, frames,
                                    wb_synth_note, tr->voice);
        /* render the instrument */
        wb_synth_render_block(tr->voice, tr->bufL, tr->bufR, frames);
    }
}

wb_engine *wb_engine_create(void) {
    wb_engine *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    wb_cmd_queue_init(&e->queue);
    e->t.bpm = 120.0;
    e->t.sample_rate = WB_SAMPLE_RATE;
    e->t.time_sig_num = 4;
    e->t.time_sig_den = 4;
    e->acc_cap = WB_MAX_BLOCK;
    e->accL = malloc(e->acc_cap * sizeof(wb_sample));
    e->accR = malloc(e->acc_cap * sizeof(wb_sample));
    return e;
}

void wb_engine_destroy(wb_engine *e) {
    if (!e) return;
    if (e->rtracks) {
        for (int i = 0; i < (int)WB_MAX_TRACKS; i++) {
            if (e->rtracks[i].voice) wb_synth_destroy(e->rtracks[i].voice);
            free(e->rtracks[i].bufL);
            free(e->rtracks[i].bufR);
        }
        free(e->rtracks);
    }
    free(e->accL);
    free(e->accR);
    free(e);
}

void wb_engine_set_session(wb_engine *e, wb_session *s) {
    e->session = s;
    if (s) {
        e->t.bpm = s->bpm;
        e->t.time_sig_num = s->time_sig_num;
        e->t.time_sig_den = s->time_sig_den;
    }
    if (e->rtracks) {
        for (int i = 0; i < (int)WB_MAX_TRACKS; i++) {
            if (e->rtracks[i].voice) wb_synth_destroy(e->rtracks[i].voice);
            free(e->rtracks[i].bufL); free(e->rtracks[i].bufR);
        }
        free(e->rtracks);
        e->rtracks = NULL;
    }
    if (!s) return;
    e->rtracks = calloc(WB_MAX_TRACKS, sizeof(wb_track_runtime));
    for (uint32_t i = 0; i < s->track_count; i++) {
        wb_track_runtime *tr = &e->rtracks[i];
        tr->active = 1;
        tr->kind = s->tracks[i].kind;
        tr->volume = s->tracks[i].volume;
        tr->pan = s->tracks[i].pan;
        tr->mute = s->tracks[i].mute;
        tr->solo = s->tracks[i].solo;
        tr->buf_cap = WB_MAX_BLOCK;
        tr->bufL = malloc(WB_MAX_BLOCK * sizeof(wb_sample));
        tr->bufR = malloc(WB_MAX_BLOCK * sizeof(wb_sample));
        if (s->tracks[i].kind == 0)
            tr->voice = wb_synth_create(WB_SAMPLE_RATE);
    }
}

wb_session *wb_engine_get_session(wb_engine *e) { return e->session; }

void wb_engine_play(wb_engine *e) { wb_cmd c = { .type = WB_CMD_PLAY }; wb_cmd_push(&e->queue, c); }
void wb_engine_stop(wb_engine *e) { wb_cmd c = { .type = WB_CMD_STOP }; wb_cmd_push(&e->queue, c); }
void wb_engine_seek(wb_engine *e, double p) { wb_cmd c = { .type = WB_CMD_SEEK, .f0 = p }; wb_cmd_push(&e->queue, c); }
void wb_engine_set_bpm(wb_engine *e, double bpm) { wb_cmd c = { .type = WB_CMD_SET_BPM, .f0 = bpm }; wb_cmd_push(&e->queue, c); }
void wb_engine_get_transport(wb_engine *e, wb_transport *out) { if (out) *out = e->t; }
void wb_engine_set_track_volume(wb_engine *e, int track, float vol) {
    wb_cmd c = { .type = WB_CMD_SET_TRACK_VOL, .i0 = track, .f0 = vol };
    wb_cmd_push(&e->queue, c);
}
void wb_engine_note(wb_engine *e, int track, uint8_t pitch, uint8_t vel) {
    wb_cmd c = { .type = WB_CMD_NOTE, .i0 = track, .i1 = pitch, .f0 = vel };
    wb_cmd_push(&e->queue, c);
}
void wb_engine_set_insert_param(wb_engine *e, int track, int slot, int param, float value) {
    (void)e; (void)track; (void)slot; (void)param; (void)value;
}

uint32_t wb_engine_render(wb_engine *e, wb_sample *out, uint32_t n) {
    if (!out || n == 0) return 0;
    if (n > e->acc_cap) return 0;

    engine_process_cmds(e);
    memset(out, 0, n * 2 * sizeof(wb_sample));

    int any_solo = 0;
    if (e->session && e->rtracks)
        for (uint32_t t = 0; t < e->session->track_count; t++)
            if (e->rtracks[t].solo) any_solo = 1;

    if (e->session && e->rtracks) {
        for (uint32_t t = 0; t < e->session->track_count; t++) {
            wb_track_runtime *tr = &e->rtracks[t];
            if (!tr->active || tr->mute) continue;
            if (any_solo && !tr->solo) continue;

            render_track(e, t, n);

            float l = (float)(1.0 - (tr->pan > 0 ? tr->pan : 0));
            float r = (float)(1.0 - (tr->pan < 0 ? -tr->pan : 0));
            float g = tr->volume;
            for (uint32_t i = 0; i < n; i++) {
                out[2*i]   += tr->bufL[i] * g * l;
                out[2*i+1] += tr->bufR[i] * g * r;
            }
        }
    }

    if (e->t.playing) {
        e->t.song_pos += n;
        if (e->t.loop_on && (e->t.loop_end - e->t.loop_start) > 0) {
            if (e->t.song_pos >= e->t.loop_end)
                e->t.song_pos = e->t.loop_start +
                    fmod(e->t.song_pos - e->t.loop_start,
                         e->t.loop_end - e->t.loop_start);
        }
    }
    return n;
}

float wb_engine_cpu_load(wb_engine *e) { return e->cpu_load; }

int wb_engine_render_session(wb_engine *e, wb_session *s, wb_sample **out, uint32_t *frames) {
    if (!s || s->length <= 0) return -1;
    wb_engine *tmp = wb_engine_create();
    wb_engine_set_session(tmp, s);
    wb_engine_seek(tmp, 0);
    tmp->t.playing = 1;   /* force transport advance */

    uint32_t total = (uint32_t)s->length;
    wb_sample *buf = malloc(total * 2 * sizeof(wb_sample));
    if (!buf) { wb_engine_destroy(tmp); return -1; }

    uint32_t done = 0;
    while (done < total) {
        uint32_t n = total - done;
        if (n > WB_MAX_BLOCK) n = WB_MAX_BLOCK;
        wb_engine_render(tmp, buf + done*2, n);
        done += n;
    }
    wb_engine_destroy(tmp);
    *out = buf;
    *frames = total;
    return 0;
}
