/* wb_recorder.c — MIDI record-into-clip capture.
 *
 * Captures live MIDI note-on/note-off events into the engine's current clip for
 * a track. Each note is timestamped with the engine's song position (samples);
 * note-offs close out the matching pending note-on, computing duration.
 *
 * Thread model (R002): the MIDI callback (CoreMIDI receive thread) and the
 * engine render callback both need access. We use a single-slot pending-note
 * table keyed by pitch — write from the MIDI thread, read/clear from the render
 * thread (once per block). The note-on record itself only pushes to the clip's
 * growing note array (realloc is done on the render thread via a pending queue,
 * never on the RT MIDI callback path).
 *
 * Pure C11.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "wbus.h"
#include "wb_recorder.h"

#define WB_MAX_PENDING_NOTES 128

typedef struct {
    int   active;     /* note-on seen, note-off pending */
    float pitch;
    double start;    /* song position at note-on (samples) */
} rec_pending;

struct wb_recorder {
    wb_track   *track;     /* track whose clip we record into */
    int          clip_idx; /* which clip on the track */
    int          overdub;  /* 1 = keep existing notes; 0 = replace clip */
    rec_pending  pending[WB_MAX_PENDING_NOTES];
    /* deferred note-off queue (written by RT MIDI thread, flushed by render) */
    struct { float pitch; double end; } off_q[WB_MAX_PENDING_NOTES];
    int off_head, off_tail;
};

wb_recorder *wb_recorder_create(wb_track *tr, int clip_idx) {
    if (!tr) return NULL;
    wb_recorder *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->track = tr;
    r->clip_idx = clip_idx;
    return r;
}

void wb_recorder_destroy(wb_recorder *r) { free(r); }

void wb_recorder_set_overdub(wb_recorder *r, int overdub) {
    if (r) r->overdub = overdub;
}

/* Called from the RT MIDI callback thread. Only appends to fixed-size queues;
 * never reallocs. Returns 0 on success, -1 if overflow (note dropped). */
int wb_recorder_midi_event(wb_recorder *r, int pitch, int vel, double song_pos) {
    if (!r) return -1;
    if (vel > 0) {
        /* note-on: find a free pending slot for this pitch */
        for (int i = 0; i < WB_MAX_PENDING_NOTES; i++) {
            if (!r->pending[i].active) {
                r->pending[i].active  = 1;
                r->pending[i].pitch   = (float)pitch;
                r->pending[i].start   = song_pos;
                return 0;
            }
        }
        return -1; /* all slots full */
    } else {
        /* note-off: match and queue a release timestamp */
        for (int i = 0; i < WB_MAX_PENDING_NOTES; i++) {
            if (r->pending[i].active && (int)r->pending[i].pitch == pitch) {
                int h = r->off_head;
                int next = (h + 1) % WB_MAX_PENDING_NOTES;
                if (next == r->off_tail) return -1; /* off queue full */
                r->off_q[h].pitch = (float)pitch;
                r->off_q[h].end   = song_pos;
                r->off_head = next;
                r->pending[i].active = 0;
                return 0;
            }
        }
        return -1; /* no pending note-on for this pitch */
    }
}

/* Flush deferred note-ons/offs into the target clip. Called once per render
 * block (from wb_engine_render). Performs the reallocs here, NOT on the RT
 * MIDI thread. Returns the number of notes added. */
int wb_recorder_flush(wb_recorder *r, double song_pos_end) {
    if (!r || !r->track || r->clip_idx < 0) return 0;
    wb_clip *cl = &r->track->clips[r->clip_idx];

    /* first-touch: if not overdubbing, reset the clip */
    if (!r->overdub && cl->notes == NULL && cl->note_count == 0) {
        /* keep clip; just start recording into it */
    }

    for (int i = 0; i < WB_MAX_PENDING_NOTES; i++) {
        if (!r->pending[i].active) continue;
        /* a note-on still hanging past the flush boundary gets a default dur */
        double end = song_pos_end;
        if (end < r->pending[i].start + 1.0)
            end = r->pending[i].start + 1.0;
        wb_note no;
        memset(&no, 0, sizeof(no));
        no.pitch = (uint8_t)r->pending[i].pitch;
        no.start = r->pending[i].start;
        no.dur   = end - r->pending[i].start;
        no.vel   = 100;
        cl->notes = realloc(cl->notes, (cl->note_count + 1) * sizeof(wb_note));
        if (cl->notes) cl->notes[cl->note_count++] = no;
        r->pending[i].active = 0;
    }

    /* process deferred note-offs */
    int added = 0;
    while (r->off_tail != r->off_head) {
        int idx = r->off_tail;
        float p = r->off_q[idx].pitch;
        double end = r->off_q[idx].end;
        r->off_tail = (r->off_tail + 1) % WB_MAX_PENDING_NOTES;
        /* find matching recorded note-on and close its duration */
        for (uint32_t n = 0; n < cl->note_count; n++) {
            if (cl->notes[n].pitch == (uint8_t)p && cl->notes[n].dur <= 0) {
                cl->notes[n].dur = end - cl->notes[n].start;
                if (cl->notes[n].dur < 0) cl->notes[n].dur = 0;
                added++;
                break;
            }
        }
    }
    return added;
}
