/*
 * wb_midi.c — Standard MIDI File writer (strict C11, no third party).
 * Format 1: header + one track. Variable-length quantities, running
 * status omitted (explicit status bytes everywhere) for simplicity.
 */
#include "wb_midi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u32(unsigned char *b, unsigned long v) {
    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)(v);
}

static void put_u16(unsigned char *b, unsigned v) {
    b[0] = (unsigned char)(v >> 8);
    b[1] = (unsigned char)(v);
}

/* variable-length quantity */
static int put_vlq(unsigned char *b, unsigned long v) {
    int n = 0;
    unsigned char tmp[4];
    do {
        tmp[n++] = (unsigned char)(v & 0x7F);
        v >>= 7;
    } while (v);
    /* reverse */
    for (int i = 0; i < n; i++) b[i] = tmp[n - 1 - i] | (i < n - 1 ? 0x80 : 0x00);
    return n;
}

typedef struct {
    unsigned char *data;
    size_t len, cap;
    unsigned long tick;  /* current absolute tick */
} track_t;

static void tr_push(track_t *t, const unsigned char *b, size_t n) {
    if (t->len + n > t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 1024;
        while (t->len + n > nc) nc *= 2;
        t->data = realloc(t->data, nc);
        t->cap = nc;
    }
    memcpy(t->data + t->len, b, n);
    t->len += n;
}

static void tr_event(track_t *t, unsigned long delta_ticks,
                     const unsigned char *ev, size_t evlen) {
    unsigned char vlq[4];
    int nv = put_vlq(vlq, delta_ticks);
    tr_push(t, vlq, (size_t)nv);
    tr_push(t, ev, evlen);
    t->tick += delta_ticks;
}

static void tr_note(track_t *t, unsigned long abs_tick, int on,
                    int note, int vel) {
    unsigned long dt = abs_tick > t->tick ? abs_tick - t->tick : 0;
    unsigned char ev[3];
    ev[0] = (unsigned char)(0x90 | 0);  /* channel 1 */
    ev[1] = (unsigned char)(note & 0x7F);
    ev[2] = (unsigned char)(on ? (vel & 0x7F) : 0);
    tr_event(t, dt, ev, 3);
}

int wb_midi_write(const char *path, const wb_midi_note_t *notes, int nnotes,
                  int bpm, int ppq) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    /* ---- header chunk ---- */
    fwrite("MThd", 1, 4, f);
    unsigned char h[10];
    put_u32(h, 6);          /* header length */
    put_u16(h + 4, 1);      /* format 1 */
    put_u16(h + 6, 1);      /* one track */
    put_u16(h + 8, ppq);    /* pulses per quarter note */
    fwrite(h, 1, 10, f);

    /* ---- track chunk ---- */
    track_t tr = { 0 };
    /* tempo meta event at tick 0 (bpm -> microseconds per quarter) */
    {
        unsigned long uspq = bpm > 0 ? 60000000UL / (unsigned long)bpm : 500000UL;
        unsigned char ev[6];
        ev[0] = 0xFF; ev[1] = 0x51; ev[2] = 3;
        ev[3] = (unsigned char)((uspq >> 16) & 0xFF);
        ev[4] = (unsigned char)((uspq >> 8) & 0xFF);
        ev[5] = (unsigned char)(uspq & 0xFF);
        tr_event(&tr, 0, ev, 6);
    }
    /* end-of-track meta */
    for (int i = 0; i < nnotes; i++) {
        unsigned long start = (unsigned long)(notes[i].start_beats * ppq);
        unsigned long end = (unsigned long)((notes[i].start_beats + notes[i].duration_beats) * ppq);
        tr_note(&tr, start, 1, notes[i].note, notes[i].velocity);
        tr_note(&tr, end, 0, notes[i].note, 0);
    }
    unsigned char eot[3] = { 0xFF, 0x2F, 0x00 };
    tr_event(&tr, 0, eot, 3);

    fwrite("MTrk", 1, 4, f);
    unsigned char sz[4];
    put_u32(sz, (unsigned long)tr.len);
    fwrite(sz, 1, 4, f);
    fwrite(tr.data, 1, tr.len, f);

    free(tr.data);
    fclose(f);
    return 0;
}
