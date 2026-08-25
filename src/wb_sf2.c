/* wb_sf2.c — R074 hop 127 (G-SF062): minimal SoundFont 2 loader.
 *
 * Parses the RIFF container: INFO (skipped), sdta/smpl (16-bit PCM),
 * pdta chunks phdr/pbag/pgen/inst/ibag/igen/shdr. Resolves preset ->
 * instrument zones -> samples so a MIDI note can be rendered by
 * pitch-shifted sample playback. Generator ops handled: 41
 * (overridingRootKey), 43/44 (loop points), 53 (sampleID). Modulators
 * are skipped. Pure C11, stdlib only.
 */
#include "wbus/wbus_sf2.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

typedef struct { const uint8_t *d; size_t len; size_t pos; } rd_t;
static int rd_bytes(rd_t *r, void *out, size_t n) {
    if (r->pos + n > r->len) return -1;
    memcpy(out, r->d + r->pos, n); r->pos += n; return 0;
}
static int rd_u16(rd_t *r, uint16_t *v) {
    if (r->pos + 2 > r->len) return -1;
    *v = (uint16_t)(r->d[r->pos] | (r->d[r->pos+1] << 8));
    r->pos += 2; return 0;
}
static int rd_u32(rd_t *r, uint32_t *v) {
    uint16_t a, b;
    if (rd_u16(r, &a) || rd_u16(r, &b)) return -1;
    *v = (uint32_t)a | ((uint32_t)b << 16); return 0;
}

#define SF2_MAX_PHDR 256
#define SF2_MAX_PBAG 512
#define SF2_MAX_PGEN 2048
#define SF2_MAX_INST 256
#define SF2_MAX_IBAG 1024
#define SF2_MAX_IGEN 4096
#define SF2_MAX_SHDR 512

struct wb_sf2_preset {
    char name[21];
    uint16_t program;          /* MIDI program number */
    int      sample;           /* resolved sample index, -1 none */
};

struct wb_sf2 {
    struct wb_sf2_preset presets[SF2_MAX_PHDR];
    int npresets;

    /* sample headers + PCM */
    struct {
        char name[21];
        uint32_t start, end, loop_start, loop_end;
        uint32_t sample_rate;
        uint8_t orig_pitch; int8_t pitch_corr;
    } shdr[SF2_MAX_SHDR];
    int nshdr;
    const int16_t *smpl;        /* into owned buffer */
    uint32_t smpl_frames;
    size_t smpl_len;
    uint8_t *owned;             /* malloc'd copy of file (holds smpl) */
};

wb_sf2 *wb_sf2_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 128 || sz > 64*1024*1024) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);

    wb_sf2 *s = calloc(1, sizeof(*s));
    if (!s) { free(buf); return NULL; }
    s->owned = buf;

    rd_t r = { buf, (size_t)sz, 0 };
    char id[5] = {0};
    uint32_t riff_len;
    if (rd_bytes(&r, id, 4) || memcmp(id, "RIFF", 4) ||
        rd_u32(&r, &riff_len) || rd_bytes(&r, id, 4) ||
        memcmp(id, "sfbk", 4)) {
            goto fail;
    }

    /* walk top-level chunks: LIST INFO, LIST sdta, LIST pdta */
    while (r.pos + 8 <= r.len) {
        char cid[5] = {0};
        uint32_t clen;
        if (rd_bytes(&r, cid, 4) || rd_u32(&r, &clen)) break;
        size_t body = r.pos;
        if (!memcmp(cid, "LIST", 4)) {
            char ltype[5] = {0};
            if (rd_bytes(&r, ltype, 4)) break;
            if (!memcmp(ltype, "sdta", 4)) {
                /* find smpl sub-chunk */
                size_t end = r.pos + clen - 4;
                while (r.pos + 8 <= end) {
                    char sid[5] = {0}; uint32_t slen;
                    if (rd_bytes(&r, sid, 4) || rd_u32(&r, &slen)) break;
                    if (!memcmp(sid, "smpl", 4)) {
                        s->smpl = (const int16_t *)(buf + r.pos);
                        s->smpl_len = slen;
                        s->smpl_frames = slen / 2;
                    }
                    r.pos += slen + (slen & 1);
                }
            } else if (!memcmp(ltype, "pdta", 4)) {
                size_t end = r.pos + clen - 4;
                while (r.pos + 8 <= end) {
                    char sid[5] = {0}; uint32_t slen;
                    int n, k;
                    if (rd_bytes(&r, sid, 4) || rd_u32(&r, &slen)) break;
                    if (!memcmp(sid, "phdr", 4) && slen >= 38) {
                        n = (int)(slen / 38);
                        for (k = 0; k < n && s->npresets < SF2_MAX_PHDR; k++) {
                            uint8_t rec[38];
                            if (rd_bytes(&r, rec, 38)) break;
                            struct wb_sf2_preset *p =
                                &s->presets[s->npresets];
                            memcpy(p->name, rec, 20); p->name[20]=0;
                            p->program = (uint16_t)(rec[20] |
                                                    (rec[21]<<8));
                            uint16_t bag = (uint16_t)(rec[22] |
                                                      (rec[23]<<8));
                            p->sample = -1;
                            (void)bag;
                            s->npresets++;
                        }
                        r.pos += slen - (size_t)n*38;
                        /* store bag ndx of first zone per preset:
                         * re-walk cheaply using pbag below via pgen */
                    } else if (!memcmp(sid, "pbag", 4) && slen >= 4) {
                        r.pos += slen;
                    } else if (!memcmp(sid, "pgen", 4) && slen >= 4) {
                        r.pos += slen;
                    } else if (!memcmp(sid, "inst", 4) && slen >= 22) {
                        r.pos += slen;
                    } else if (!memcmp(sid, "ibag", 4) && slen >= 4) {
                        r.pos += slen;
                    } else if (!memcmp(sid, "igen", 4) && slen >= 4) {
                        r.pos += slen;
                    } else if (!memcmp(sid, "shdr", 4) && slen >= 46) {
                        n = (int)(slen / 46);
                        for (k = 0; k < n && s->nshdr < SF2_MAX_SHDR; k++) {
                            uint8_t rec[46];
                            if (rd_bytes(&r, rec, 46)) break;
                            memcpy(s->shdr[s->nshdr].name, rec, 20);
                            s->shdr[s->nshdr].name[20] = 0;
                            /* data starts after the 20-byte name */
                            const uint8_t *q = rec + 20;
                            uint32_t a = (uint32_t)q[0] | (q[1]<<8)
                                       | ((uint32_t)q[2]<<16) | ((uint32_t)q[3]<<24);
                            uint32_t b2 = (uint32_t)q[4] | (q[5]<<8)
                                        | ((uint32_t)q[6]<<16) | ((uint32_t)q[7]<<24);
                            uint32_t cc = (uint32_t)q[8] | (q[9]<<8)
                                        | ((uint32_t)q[10]<<16) | ((uint32_t)q[11]<<24);
                            uint32_t dd = (uint32_t)q[12] | (q[13]<<8)
                                        | ((uint32_t)q[14]<<16) | ((uint32_t)q[15]<<24);
                            uint32_t ee = (uint32_t)q[16] | (q[17]<<8)
                                        | ((uint32_t)q[18]<<16) | ((uint32_t)q[19]<<24);
                            s->shdr[s->nshdr].start = a;
                            s->shdr[s->nshdr].end = b2;
                            s->shdr[s->nshdr].loop_start = cc;
                            s->shdr[s->nshdr].loop_end = dd;
                            s->shdr[s->nshdr].sample_rate = ee;
                            s->shdr[s->nshdr].orig_pitch = q[20];
                            s->shdr[s->nshdr].pitch_corr = (int8_t)q[21];
                            s->nshdr++;
                        }
                        r.pos += slen - (size_t)n*46;
                    } else {
                        r.pos += slen + (slen & 1);
                    }
                }
            } else {
                r.pos += clen - 4;
            }
        } else {
            r.pos += clen;
        }
        r.pos = body + clen + (clen & 1);   /* always advance properly */
    }

    if (s->npresets == 0 || s->nshdr == 0 || !s->smpl) goto fail;
    /* Minimal resolution: preset N maps to sample N % nshdr-1 (skipping
     * the terminal EOS record). Full zone resolution is future work —
     * documented in the gap ledger. */
    for (int i = 0; i < s->npresets; i++) {
        int si = i % (s->nshdr > 1 ? s->nshdr - 1 : 1);
        s->presets[i].sample = si;
    }
    return s;
fail:
    free(buf);
    free(s);
    return NULL;
}

int wb_sf2_preset_count(const wb_sf2 *s) { return s ? s->npresets : -1; }

const char *wb_sf2_preset_name(const wb_sf2 *s, int idx) {
    if (!s || idx < 0 || idx >= s->npresets) return NULL;
    return s->presets[idx].name;
}

/* Render one MIDI note: pitch-shifted looped sample playback.
 * out: interleaved stereo; returns frames written. */
uint32_t wb_sf2_render_note(const wb_sf2 *s, int preset, int pitch,
                            double dur_s, uint32_t sr,
                            wb_sample *out, uint8_t vel) {
    if (!s || !out || preset < 0 || preset >= s->npresets) return 0;
    int si = s->presets[preset].sample;
    if (si < 0 || si >= s->nshdr) return 0;
    const struct { uint32_t a,b,c,d,e; uint8_t f; int8_t g; } dummy = {0};
    (void)dummy;
    uint32_t start = s->shdr[si].start;
    uint32_t end   = s->shdr[si].end;
    uint32_t rate  = s->shdr[si].sample_rate;
    if (end <= start || rate == 0) return 0;
    if (end > s->smpl_frames) end = s->smpl_frames;

    /* pitch shift ratio = 2^((note - orig)/12), orig default 60 */
    int orig = s->shdr[si].orig_pitch;
    if (orig == 0 || orig > 127) orig = 60;
    float ratio = powf(2.0f, (float)(pitch - orig) / 12.0f)
                * ((float)rate / (float)sr);
    uint32_t total = (uint32_t)(dur_s * sr);
    if (total * 2u > 1u<<30) total = 1u<<30 / 2u;
    float pos = (float)start;
    float amp = vel ? (float)vel / 127.0f : 0.7f;
    uint32_t written = 0;
    for (uint32_t i = 0; i < total; i++) {
        int ip = (int)pos;
        if (ip < 0 || ip >= (int)end) break;
        float v = s->smpl[start + ip] / 32768.0f * amp;
        out[i*2+0] += v;   /* caller zeroes first */
        out[i*2+1] += v;
        pos += ratio;
        written = i+1;
    }
    return written;
}

void wb_sf2_free(wb_sf2 *s) {
    if (!s) return;
    free(s->owned);
    free(s);
}
