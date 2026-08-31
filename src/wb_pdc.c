/* src/wb_pdc.c — plugin delay compensation (PDC).
 * Automatic latency compensation for plugin chains. Each track reports the sum
 * of its insert-slot latencies; PDC delays the non-latency tracks to match the
 * longest track so everything stays sample-aligned at the mix bus.
 *
 * Pure C11, zero third-party. Standalone module — no engine dependency so it
 * can be unit-tested without a real wb_engine*.
 */
#include <stdlib.h>
#include <string.h>
#include "wbus.h"

#ifndef WB_PDC_MAX_TRACKS
#define WB_PDC_MAX_TRACKS 128
#endif

/* Per-track delay ring buffer + bookkeeping. */
typedef struct wb_pdc_track {
    int      latency_samples;   /* sum of insert latencies (reported) */
    int      delay_samples;     /* applied delay = max_latency - latency */
    int      rb_cap;            /* ring buffer capacity (>= delay_samples) */
    int      rb_pos;            /* write cursor */
    wb_sample *rb;              /* ring buffer (capacity + 1 for safety) */
} wb_pdc_track;

struct wb_pdc {
    int             num_tracks;
    uint32_t        sample_rate;
    int             enabled;
    int             max_latency; /* max(latency across tracks) */
    wb_pdc_track    tracks[WB_PDC_MAX_TRACKS];
};

/* ---- helpers ----------------------------------------------------------- */

static void wb_pdc_recompute(wb_pdc *p)
{
    p->max_latency = 0;
    for (int i = 0; i < p->num_tracks; i++) {
        if (p->tracks[i].latency_samples > p->max_latency)
            p->max_latency = p->tracks[i].latency_samples;
    }
    for (int i = 0; i < p->num_tracks; i++) {
        int needed = p->max_latency - p->tracks[i].latency_samples;
        if (needed < 0) needed = 0;
        p->tracks[i].delay_samples = needed;

        /* Grow ring buffer if the required delay increased. */
        if (needed + 1 > p->tracks[i].rb_cap) {
            int new_cap = needed + 1;
            wb_sample *nr = (wb_sample *)calloc((size_t)new_cap, sizeof(wb_sample));
            if (nr) {
                free(p->tracks[i].rb);
                p->tracks[i].rb = nr;
                p->tracks[i].rb_cap = new_cap;
                p->tracks[i].rb_pos = 0;
            }
        }
        /* If the delay shrank to zero we keep the buffer (harmless); if the
         * new delay is smaller than the old we just don't read as far back. */
    }
}

/* ---- public API -------------------------------------------------------- */

wb_pdc *wb_pdc_create(int num_tracks, uint32_t sr)
{
    if (num_tracks <= 0 || num_tracks > WB_PDC_MAX_TRACKS) return NULL;
    wb_pdc *p = (wb_pdc *)calloc(1, sizeof(wb_pdc));
    if (!p) return NULL;
    p->num_tracks = num_tracks;
    p->sample_rate = sr;
    p->enabled = 1;
    p->max_latency = 0;
    /* tracks[] zeroed by calloc (latency=0, delay=NULL rb, etc.). */
    return p;
}

void wb_pdc_destroy(wb_pdc *p)
{
    if (!p) return;
    for (int i = 0; i < p->num_tracks; i++)
        free(p->tracks[i].rb);
    free(p);
}

void wb_pdc_set_latency(wb_pdc *p, int track, int samples)
{
    if (!p || track < 0 || track >= p->num_tracks) return;
    if (samples < 0) samples = 0;
    p->tracks[track].latency_samples = samples;
    wb_pdc_recompute(p);
}

int wb_pdc_get_delay(const wb_pdc *p, int track)
{
    if (!p || track < 0 || track >= p->num_tracks) return 0;
    return p->tracks[track].delay_samples;
}

int wb_pdc_get_max_latency(const wb_pdc *p)
{
    if (!p) return 0;
    return p->max_latency;
}

void wb_pdc_set_enabled(wb_pdc *p, int enabled)
{
    if (!p) return;
    p->enabled = enabled ? 1 : 0;
}

int wb_pdc_is_enabled(const wb_pdc *p)
{
    return p ? p->enabled : 0;
}

/* Delay one track's buffer by track's delay_samples using its ring buffer.
 * When disabled (or delay==0) the buffer is passed through unmodified. */
static void wb_pdc_delay_track(wb_pdc *p, int track, wb_sample *buf, uint32_t frames)
{
    wb_pdc_track *tr = &p->tracks[track];
    if (!p->enabled || tr->delay_samples == 0) return;
    int d = tr->delay_samples;
    int cap = tr->rb_cap;
    /* Ring buffer holds delayed samples. We write current input, then read
     * d samples behind — that's the delayed output. */
    for (uint32_t i = 0; i < frames; i++) {
        /* Write current input sample at cursor. */
        tr->rb[tr->rb_pos] = buf[i];
        /* Read position is d steps behind the write cursor. */
        int read_pos = tr->rb_pos - d;
        if (read_pos < 0) read_pos += cap;
        buf[i] = tr->rb[read_pos];
        /* Advance write cursor. */
        tr->rb_pos++;
        if (tr->rb_pos >= cap) tr->rb_pos = 0;
    }
}

void wb_pdc_process(wb_pdc *p, wb_sample **buffers, int num_tracks, uint32_t frames)
{
    if (!p || !buffers) return;
    int n = num_tracks < p->num_tracks ? num_tracks : p->num_tracks;
    for (int i = 0; i < n; i++)
        wb_pdc_delay_track(p, i, buffers[i], frames);
}

/* ---- engine-facing wrappers -------------------------------------------- */
/* These call into the standalone PDC when the engine has one attached, else
 * they are no-ops (the engine keeps its own PDC state). For now we expose the
 * API symbols so the engine can link them; the real engine integration will
 * cache a wb_pdc* inside wb_engine. */

int wb_engine_set_plugin_latency(wb_engine *engine, int track, int slot, int latency_samples)
{
    (void)engine; (void)slot;
    /* Engine would sum per-slot latencies into the track's total. We don't
     * have a real engine pointer in this standalone build, so this is a
     * placeholder that would be wired to engine->pdc in the full integration.
     * Returns 0 on success. */
    if (!engine || track < 0 || slot < 0 || latency_samples < 0) return -1;
    return 0;
}

int wb_engine_get_track_latency(wb_engine *engine, int track)
{
    (void)engine; (void)track;
    return 0;
}

int wb_engine_get_max_latency(wb_engine *engine)
{
    (void)engine;
    return 0;
}

void wb_engine_apply_pdc(wb_engine *engine, wb_sample **track_buffers,
                         int num_tracks, uint32_t frames)
{
    (void)engine; (void)track_buffers; (void)num_tracks; (void)frames;
}

int wb_engine_set_pdc_enabled(wb_engine *engine, int enabled)
{
    (void)engine; (void)enabled;
    return 0;
}