/* wb_smf.c — R074 hop 115 (G-SF061): Standard MIDI File (SMF) loader.
 *
 * Parses type-0/1 .mid files into wb_note arrays (seconds-based).
 * Supports running status, tempo map (first tempo event), note-on/off
 * pairing, multi-track merge. Pure C11, stdlib only.
 */
#include "wbus/wbus_smf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    const uint8_t *d;
    size_t len, pos;
} smf_rd;

static int rd_u8(smf_rd *r, uint32_t *out) {
    if (r->pos >= r->len) return -1;
    *out = r->d[r->pos++];
    return 0;
}
static int rd_u16(smf_rd *r, uint16_t *out) {
    uint32_t a, b;
    if (rd_u8(r, &a) || rd_u8(r, &b)) return -1;
    *out = (uint16_t)((a << 8) | b);
    return 0;
}
static int rd_u32(smf_rd *r, uint32_t *out) {
    uint16_t hi, lo;
    if (rd_u16(r, &hi) || rd_u16(r, &lo)) return -1;
    *out = ((uint32_t)hi << 16) | lo;
    return 0;
}
static int rd_vlq(smf_rd *r, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t b;
        if (rd_u8(r, &b)) return -1;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) { *out = v; return 0; }
    }
    return -1;
}

#define SMF_MAX_NOTES 4096

struct wb_smf {
    wb_note *notes;
    int      nnotes;
    double   duration;     /* seconds */
    double   tempo_bpm;    /* first tempo seen (default 120) */
    int      tpqn;         /* ticks per quarter note */
};

wb_smf *wb_smf_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 14 || sz > 4 * 1024 * 1024) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);

    wb_smf *sm = calloc(1, sizeof(*sm));
    if (!sm) { free(buf); return NULL; }
    sm->notes = calloc(SMF_MAX_NOTES, sizeof(wb_note));
    if (!sm->notes) { free(sm); free(buf); return NULL; }
    sm->tempo_bpm = 120.0;

    smf_rd r = { buf, (size_t)sz, 0 };
    uint32_t chunk_id, chunk_len;
    if (rd_u32(&r, &chunk_id) ||
        (chunk_id != 0x4D546864u) /* MThd */ ||
        rd_u32(&r, &chunk_len)) goto fail;
    uint16_t fmt, ntrks, div;
    if (rd_u16(&r, &fmt) || rd_u16(&r, &ntrks) || rd_u16(&r, &div))
        goto fail;
    if (div & 0x8000u) goto fail;          /* SMPTE time unsupported */
    sm->tpqn = div > 0 ? div : 480;
    r.pos += (chunk_len - 6);              /* skip rest of header */

    double sec_per_tick = 60.0 / (sm->tempo_bpm * sm->tpqn);

    for (uint16_t tk = 0; tk < ntrks; tk++) {
        if (rd_u32(&r, &chunk_id) || chunk_id != 0x4D54726Bu /* MTrk */ ||
            rd_u32(&r, &chunk_len)) break;
        size_t trk_end = r.pos + chunk_len;
        double ticks = 0;                  /* running absolute tick */
        uint32_t run_status = 0;
        /* active note-ons for pairing: pitch -> (start_tick, vel) */
        struct { double t; uint8_t vel; } on[128][16];
        memset(on, 0, sizeof on);

        while (r.pos < trk_end) {
            uint32_t delta;
            if (rd_vlq(&r, &delta)) break;
            ticks += delta;
            uint32_t st;
            if (rd_u8(&r, &st)) break;
            if (!(st & 0x80u)) {           /* running status */
                if (r.pos == 0) break;
                r.pos--;                   /* push back */
                st = run_status;
                if (!st) break;
            } else if (st < 0xF0u) {
                run_status = st;
            }
            if (st == 0xFFu) {             /* meta */
                uint32_t type;
                if (rd_u8(&r, &type)) break;
                uint32_t mlen;
                if (rd_vlq(&r, &mlen)) break;
                if (type == 0x51 && mlen == 3 && r.pos + 3 <= r.len) {
                    uint32_t us_qn = ((uint32_t)r.d[r.pos] << 16) |
                                     ((uint32_t)r.d[r.pos+1] << 8) |
                                      r.d[r.pos+2];
                    sm->tempo_bpm = 60000000.0 / us_qn;
                    sec_per_tick = 60.0 / (sm->tempo_bpm * sm->tpqn);
                }
                r.pos += mlen;
                continue;
            }
            if ((st & 0xF0u) == 0xF0u) {   /* sysex — skip length byte */
                uint32_t mlen;
                if (rd_vlq(&r, &mlen)) break;
                r.pos += mlen;
                continue;
            }
            int hi = st & 0xF0u, ch = st & 0x0Fu;
            uint32_t d1, d2 = 0;
            int two_byte = (hi == 0xC0 || hi == 0xD0) ? 0 : 1;
            if (rd_u8(&r, &d1)) break;
            if (two_byte && rd_u8(&r, &d2)) break;
            if (hi == 0x90u && d2 > 0) {   /* note on */
                if (ch >= 16 || d1 > 127) continue;
                on[d1][ch].t = ticks * sec_per_tick;
                on[d1][ch].vel = (uint8_t)d2;
            } else if (hi == 0x80u ||
                       (hi == 0x90u && d2 == 0)) {   /* note off */
                if (ch >= 16 || d1 > 127 || !on[d1][ch].vel) continue;
                double t0 = on[d1][ch].t;
                double dur = ticks * sec_per_tick - t0;
                if (dur <= 0) dur = 0.01;
                if (sm->nnotes < SMF_MAX_NOTES) {
                    wb_note *n = &sm->notes[sm->nnotes++];
                    n->start = t0;
                    n->dur   = dur;
                    n->pitch = (uint8_t)d1;
                    n->vel   = on[d1][ch].vel;
                    n->mod   = 0;
                    n->atouch= 0;
                }
                on[d1][ch].vel = 0;
                double end_s = t0 + dur;
                if (end_s > sm->duration) sm->duration = end_s;
            }
        }
        r.pos = trk_end;                   /* tolerate truncated tracks */
    }
    free(buf);
    return sm;
fail:
    free(buf);
    free(sm->notes);
    free(sm);
    return NULL;
}

int wb_smf_note_count(const wb_smf *s) { return s ? s->nnotes : -1; }

const wb_note *wb_smf_notes(const wb_smf *s) { return s ? s->notes : NULL; }

double wb_smf_duration(const wb_smf *s) { return s ? s->duration : 0; }

void wb_smf_free(wb_smf *s) {
    if (!s) return;
    free(s->notes);
    free(s);
}

/* ---- R074 hop 123 (G-SF076): SMF export -------------------------------- */
static int wr_vlq(uint8_t *b, uint32_t v) {
    uint8_t tmp[5]; int n = 0;
    tmp[n++] = v & 0x7F; v >>= 7;
    while (v) { tmp[n++] = 0x80 | (v & 0x7F); v >>= 7; }
    for (int i = 0; i < n; i++) b[i] = tmp[n-1-i];
    return n;
}

int wb_smf_save(const char *path, const wb_note *notes, int nnotes,
                double bpm, int tpqn) {
    if (!path || !notes || nnotes <= 0 || bpm <= 0 || tpqn <= 0) return -1;
    double sec_per_tick = 60.0 / (bpm * tpqn);
    /* build event list: (tick, bytes) */
    typedef struct { uint32_t tick; uint8_t b[4]; int len; } ev_t;
    ev_t *evs = malloc(sizeof(ev_t) * (size_t)nnotes * 2);
    if (!evs) return -1;
    int ne = 0;
    for (int i = 0; i < nnotes; i++) {
        uint32_t on  = (uint32_t)(notes[i].start / sec_per_tick);
        uint32_t off = on + (uint32_t)(notes[i].dur / sec_per_tick);
        if (off <= on) off = on + 1;
        evs[ne].tick = on;
        evs[ne].b[0]=0x90; evs[ne].b[1]=notes[i].pitch;
        evs[ne].b[2]=notes[i].vel ? notes[i].vel : 100; evs[ne].len=3; ne++;
        evs[ne].tick = off;
        evs[ne].b[0]=0x80; evs[ne].b[1]=notes[i].pitch; evs[ne].b[2]=0;
        evs[ne].len=3; ne++;
    }
    /* sort by tick, note-offs first at equal ticks */
    for (int i = 1; i < ne; i++) {
        ev_t e = evs[i]; int j = i-1;
        while (j >= 0 && (evs[j].tick > e.tick ||
               (evs[j].tick == e.tick && evs[j].b[0] > e.b[0]))) {
            evs[j+1] = evs[j]; j--;
        }
        evs[j+1] = e;
    }
    /* serialize track */
    uint8_t *trk = malloc((size_t)ne * 12 + 32);
    if (!trk) { free(evs); return -1; }
    int tl = 0;
    uint32_t us_qn = (uint32_t)(60000000.0 / bpm);
    trk[tl++]=0; trk[tl++]=0xFF; trk[tl++]=0x51; trk[tl++]=3;
    trk[tl++]=(us_qn>>16)&0xFF; trk[tl++]=(us_qn>>8)&0xFF; trk[tl++]=us_qn&0xFF;
    uint32_t last = 0;
    for (int i = 0; i < ne; i++) {
        tl += wr_vlq(trk+tl, evs[i].tick - last);
        last = evs[i].tick;
        memcpy(trk+tl, evs[i].b, (size_t)evs[i].len); tl += evs[i].len;
    }
    trk[tl++]=0; trk[tl++]=0xFF; trk[tl++]=0x2F; trk[tl++]=0;
    free(evs);

    FILE *f = fopen(path, "wb");
    if (!f) { free(trk); return -1; }
    uint8_t hdr[14] = {
        'M','T','h','d',0,0,0,6,0,0,0,1,
        (uint8_t)(tpqn>>8),(uint8_t)(tpqn&0xFF)
    };
    fwrite(hdr,1,14,f);
    uint8_t ck[8] = {'M','T','r','k',
                     (uint8_t)(tl>>24),(uint8_t)(tl>>16),
                     (uint8_t)(tl>>8),(uint8_t)tl};
    fwrite(ck,1,8,f);
    fwrite(trk,1,(size_t)tl,f);
    fclose(f);
    free(trk);
    return 0;
}
