/* wb_midifx.c — MIDI FX chain units (mf1).
 * Stateless transforms (chord/transpose/velocity) act per-event; the arpeggiator
 * is stateful: it latches held notes and emits one per clock tick.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus_midifx.h"

#define WB_MIDIFX_MAX_HELD 16
#define WB_MIDIFX_MAX_VOICES 4

struct wb_midifx {
    wb_midifx_type type;
    float p[4];                 /* unit params */
    /* arp state */
    uint8_t held[WB_MIDIFX_MAX_HELD];
    int     held_n;
    int     arp_idx;            /* next note index in the held buffer */
    int     arp_dir;            /* direction for updown pattern */
};

wb_midifx *wb_midifx_create(wb_midifx_type type) {
    wb_midifx *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->type = type;
    m->p[0] = 0.0f;
    m->held_n = 0;
    m->arp_idx = 0;
    m->arp_dir = 1;
    return m;
}

void wb_midifx_destroy(wb_midifx *m) { free(m); }

wb_midifx_type wb_midifx_get_type(const wb_midifx *m) {
    return m ? m->type : WB_MIDIFX_NONE;
}

void wb_midifx_set_param(wb_midifx *m, int param, float value) {
    if (!m || param < 0 || param > 3) return;
    m->p[param] = value;
}

/* ---- stateless helpers ------------------------------------------------- */

static int do_chord(wb_midifx *m, const wb_midifx_event *in, wb_midifx_event *out, int maxout) {
    if (maxout < 1) return 0;
    out[0] = *in;                       /* root passes through */
    int n = 1;
    /* up to 3 added voices from p0..p2 (semitone offsets, rounded) */
    int offs[3] = { (int)lroundf(m->p[0]), (int)lroundf(m->p[1]), (int)lroundf(m->p[2]) };
    for (int i = 0; i < 3 && n < maxout; i++) {
        int semi = offs[i];
        if (semi == 0) continue;       /* 0 = no added voice */
        int p = in->pitch + semi;
        if (p < 0 || p > 127) continue;
        out[n] = *in;
        out[n].pitch = (uint8_t)p;
        n++;
    }
    return n;
}

static int do_transpose(wb_midifx *m, const wb_midifx_event *in, wb_midifx_event *out, int maxout) {
    if (maxout < 1) return 0;
    int semi = (int)lroundf(m->p[0]);
    int p = in->pitch + semi;
    if (p < 0) p = 0; else if (p > 127) p = 127;
    *out = *in;
    out->pitch = (uint8_t)p;
    return 1;
}

static int do_velocity(wb_midifx *m, const wb_midifx_event *in, wb_midifx_event *out, int maxout) {
    if (maxout < 1) return 0;
    float s = m->p[0];
    if (s < 0) s = 0;
    int v = (int)lroundf(in->vel * s);
    if (v > 127) v = 127;
    *out = *in;
    out->vel = (uint8_t)v;
    return 1;
}

/* ---- arpeggiator ------------------------------------------------------- */

static void arp_latch(wb_midifx *m, const wb_midifx_event *in) {
    if (in->on) {
        /* capture velocity for later arp emission */
        m->p[3] = (float)in->vel;
        /* add to held set (dedupe) */
        for (int i = 0; i < m->held_n; i++)
            if (m->held[i] == in->pitch) return;
        if (m->held_n < WB_MIDIFX_MAX_HELD) m->held[m->held_n++] = in->pitch;
    } else {
        /* release: remove from held set */
        for (int i = 0; i < m->held_n; i++) {
            if (m->held[i] == in->pitch) {
                for (int j = i; j < m->held_n - 1; j++) m->held[j] = m->held[j + 1];
                m->held_n--;
                i--;
            }
        }
        if (m->held_n == 0) m->arp_idx = 0;
    }
}

static int arp_emit(wb_midifx *m, wb_midifx_event *out, int maxout, int vel) {
    if (m->held_n == 0 || maxout < 1) return 0;
    /* pick next note per pattern */
    int idx = m->arp_idx % m->held_n;
    uint8_t pitch = m->held[idx];
    out[0].pitch = pitch;
    out[0].vel = (uint8_t)(vel > 127 ? 127 : vel);
    out[0].on = 1;
    out[0].tick = 0;
    /* advance index (up / down / updown) */
    int pat = (int)lroundf(m->p[1]);
    if (pat == 1) {            /* down */
        m->arp_idx--;
        if (m->arp_idx < 0) m->arp_idx = m->held_n - 1;
    } else if (pat == 2) {     /* up/down */
        m->arp_idx += m->arp_dir;
        if (m->arp_idx >= m->held_n) { m->arp_dir = -1; m->arp_idx = m->held_n - 2 < 0 ? 0 : m->held_n - 2; }
        else if (m->arp_idx < 0) { m->arp_dir = 1; m->arp_idx = 1 < m->held_n ? 1 : 0; }
    } else {                   /* up (default) */
        m->arp_idx = (m->arp_idx + 1) % m->held_n;
    }
    return 1;
}

/* ---- dispatch ---------------------------------------------------------- */

int wb_midifx_process(wb_midifx *m, const wb_midifx_event *in,
                     wb_midifx_event *out, int maxout) {
    if (!m || !in || !out) return 0;
    switch (m->type) {
        case WB_MIDIFX_CHORD:     return do_chord(m, in, out, maxout);
        case WB_MIDIFX_TRANSPOSE:  return do_transpose(m, in, out, maxout);
        case WB_MIDIFX_VELOCITY:   return do_velocity(m, in, out, maxout);
        case WB_MIDIFX_ARP:
            /* latch held notes; arp emits on tick(), not per input event */
            arp_latch(m, in);
            return 0;
        default:                  *out = *in; return 1;
    }
}

int wb_midifx_tick(wb_midifx *m, int ticks, wb_midifx_event *out, int maxout) {
    if (!m || m->type != WB_MIDIFX_ARP) return 0;
    int n = 0;
    /* emit one arpeggiated note per tick */
    for (int t = 0; t < ticks && n < maxout; t++) {
        if (m->held_n == 0) break;
        /* velocity from last note-on stored in p[3] */
        n += arp_emit(m, out + n, maxout - n, (int)lroundf(m->p[3]));
    }
    return n;
}
