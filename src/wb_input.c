/* wb_input.c — G05/G06: audio/MIDI input capture.
 *
 * A lock-free single-producer/single-consumer ring buffer holding incoming
 * interleaved stereo samples. The app feeds it from the audio-input device
 * callback (opt-in via WB_AUDIO_INPUT); tests feed synthetic samples
 * directly. wb_inputring_commit_clip writes the captured span into a new
 * clip on the arrangement — mic/instrument takes become material without a
 * separate recording pass. C11, no third party.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wbus/wbus.h>

struct wb_input_ring {
    wb_sample *buf;        /* interleaved stereo, cap_frames*2 samples */
    uint32_t   cap;        /* capacity in FRAMES */
    uint32_t   w, r;       /* write/read cursors (frames, wrap freely) */
    /* R073 hop 25: DC blocker state (1-pole HPF ~15 Hz, per channel) —
     * recorded DC skews meters/LUFS and the sync gates. Single-producer
     * write path owns this state; RT-safe arithmetic only. */
    float dc_x[2], dc_y[2];
};

wb_input_ring *wb_inputring_create(uint32_t cap_frames) {
    if (cap_frames == 0) return NULL;
    wb_input_ring *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->buf = malloc((size_t)cap_frames * 2 * sizeof(wb_sample));
    if (!r->buf) { free(r); return NULL; }
    r->cap = cap_frames;
    return r;
}

void wb_inputring_destroy(wb_input_ring *r) {
    if (!r) return;
    free(r->buf);
    free(r);
}

uint32_t wb_inputring_count(const wb_input_ring *r) {
    return r ? (r->w - r->r) : 0;
}

uint32_t wb_inputring_write(wb_input_ring *r, const wb_sample *data,
                            uint32_t frames) {
    if (!r || !data) return 0;
    uint32_t avail = r->cap - (r->w - r->r);
    if (frames > avail) frames = avail;          /* drop oldest-on-full policy:
                                                    overwrite not safe SPSC */
    /* R073 hop 25: DC blocker before the ring — y[n] = x[n]-x[n-1]+R*y[n-1],
     * R = 1 - 2*pi*15/sr ≈ 0.99786 at 44.1k */
    const float R = 0.99786f;
    uint32_t written = 0;
    for (uint32_t i = 0; i < frames; i++) {
        if ((r->w + i) - r->r >= r->cap) break;      /* full: stop early */
        uint32_t idx = (r->w + i) % r->cap;
        for (int cn = 0; cn < 2; cn++) {
            float x = data[i * 2 + cn];
            float y = x - r->dc_x[cn] + R * r->dc_y[cn];
            r->dc_x[cn] = x;
            r->dc_y[cn] = y;
            r->buf[idx * 2 + cn] = y;
        }
        written++;
    }
    r->w += written;
    return written;
}

uint32_t wb_inputring_read(wb_input_ring *r, wb_sample *out, uint32_t frames) {
    if (!r || !out) return 0;
    uint32_t avail = r->w - r->r;
    if (frames > avail) frames = avail;
    uint32_t read = 0;
    while (read < frames) {
        uint32_t idx = r->r % r->cap;
        uint32_t run = frames - read;
        uint32_t to_end = r->cap - idx;
        if (run > to_end) run = to_end;
        memcpy(out + read * 2, r->buf + idx * 2,
               run * 2 * sizeof(wb_sample));
        r->r += run;
        read += run;
    }
    return read;
}

int wb_inputring_commit_clip(wb_input_ring *r, struct wb_session *s,
                             int track, double dest, uint32_t frames) {
    if (!r || !s) return -1;
    if (track < 0 || track >= (int)s->track_count) return -1;
    if (frames == 0 || frames > wb_inputring_count(r)) return -1;
    wb_sample *data = malloc((size_t)frames * 2 * sizeof(wb_sample));
    if (!data) return -1;
    uint32_t got = wb_inputring_read(r, data, frames);
    if (got != frames) { free(data); return -1; }
    wb_track *tr = &s->tracks[track];
    int rc = wb_session_add_audio_clip(tr, dest, (double)frames, data,
                                       frames, 2);
    free(data);
    return rc >= 0 ? 0 : -1;
}
