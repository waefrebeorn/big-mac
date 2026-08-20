/* wb_core.c — Big Mac DAW engine core.
 * Architecture per R002 (study of Ardour + LMMS source):
 *   - Staged render pipeline: schedule -> instruments -> effects.
 *   - RT callback NEVER blocks: try-locks the process mutex; on contention
 *     it counts an Xrun, silences output, returns immediately.
 *   - Double-buffered output (swap per block).
 *   - Zero malloc/lock/syscall on the RT path (all buffers preallocated).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>

#include "wb_unit.h"

#include "wbus.h"
#include "wbus_cmd.h"
#include "wbus_plugin.h"
#include "wbus_dsp.h"
#include "wbus_vst3.h"
#include "wbus_modulation.h"
#include "wbus_midifx.h"
#include "wb_internal.h"
#include "wb_recorder.h"

/* forward declarations: VST3 slot map (defined later in this file) */
static void *wb_vst3_slot_map[WB_MAX_TRACKS][WB_MAX_INSERT_SLOTS];
void *wb_vst3_slot_get(int track, int slot);
void wb_vst3_slot_set(int track, int slot, void *inst);
void wb_vst3_slot_clear(int track, int slot);
void wb_unit_set_param(const char *id, void *ins, const char *pname, float v01);

/* one per track at render time */
struct wb_track_runtime {
    int    active;
    float  volume, pan;
    int    mute, solo;
    int    kind;
    int    route;                     /* -1 = master, else bus track index */
    void  *voice;                    /* instrument instance (kind 0) */
    const char *voice_unit_id;       /* "synth"|"fm"|"drum"|NULL (legacy synth) */
    wb_sample *bufL, *bufR;          /* per-track block buffers */
    uint32_t   buf_cap;
    /* insert effect instances + their unit id (data-driven, any order) */
    void  *inserts[WB_MAX_INSERT_SLOTS];
    const char *insert_ids[WB_MAX_INSERT_SLOTS];
    /* per-slot state: bypass toggle + wet mix (1.0 = fully wet, 0.0 = dry).
     * default: bypassed=0, wet=1.0 (process through fully). */
    int    bypass[WB_MAX_INSERT_SLOTS];
    float  wet[WB_MAX_INSERT_SLOTS];
    /* aux send levels: send[src] = amount to send to track 'src' (0 = no send). */
    float  send[WB_MAX_TRACKS];
    /* sidechain routing: sidechainSrc[slot] = source track index whose audio
     * feeds this slot's key input (compressor sidechain). -1 = none. */
    int    sidechainSrc[WB_MAX_INSERT_SLOTS];
    /* MIDI FX chain (mf1): transforms note events before the instrument voice.
     * NULL entries are empty slots. */
    struct wb_midifx *midifx[WB_MAX_INSERT_SLOTS];
};
typedef struct wb_track_runtime wb_track_runtime;

struct wb_engine {
    wb_session    *session;          /* caller-owned */
    wb_transport   t;
    wb_cmd_queue    queue;
    wb_track_runtime *rtracks;
    wb_sample *accL, *accR;          /* master accumulation scratch */
    uint32_t   acc_cap;
    float      master_volume;
    float cpu_load;

    /* R002: Xrun detection via process try-lock */
    pthread_mutex_t process_lock;
    int   lock_initialized;
    uint64_t xruns;

    /* ---- MIDI recording (one recorder arm per track; -1 = disarmed) ---- */
    wb_recorder *recorders[WB_MAX_TRACKS];

    /* ---- CLAP plugin bridge (optional; NULL if no host) ------------------ */
    struct wb_clap_host *clap_host;

    /* ---- Modulation matrix (unified modulation; m1) --------------------- */
    wb_mod_matrix *mod;
};

/* forward a note event through a track's MIDI FX chain, then to the voice */
static void engine_route_note(wb_engine *e, int track, const wb_midifx_event *ev) {
    wb_track_runtime *tr = &e->rtracks[track];
    if (!tr->voice) return;
    wb_midifx_event buf[8];
    int n = 1;
    buf[0] = *ev;
    /* run the event through each MIDI FX unit in chain order */
    for (int s = 0; s < WB_MAX_INSERT_SLOTS; s++) {
        if (!tr->midifx[s]) continue;
        int produced = 0;
        for (int i = 0; i < n && produced < 8; i++)
            produced += wb_midifx_process(tr->midifx[s], &buf[i], buf + produced, 8 - produced);
        n = produced > 0 ? produced : n;   /* if a unit swallowed all, stay empty */
        if (n == 0) return;                /* fully consumed */
    }
    /* deliver every emitted event to the instrument voice */
    for (int i = 0; i < n && i < 8; i++) {
        int pitch = buf[i].pitch;
        int vel = buf[i].on ? buf[i].vel : 0;
        if (tr->voice_unit_id && strcmp(tr->voice_unit_id, "fm") == 0)
            wb_fm_note(tr->voice, pitch, (uint8_t)vel);
        else if (tr->voice_unit_id && strcmp(tr->voice_unit_id, "drum") == 0)
            wb_drum_note(tr->voice, pitch, (uint8_t)vel);
        else
            wb_synth_note(tr->voice, pitch, (uint8_t)vel);
    }
}

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
        case WB_CMD_SET_INSERT_BYPASS:
            if (e->rtracks && c.i0 >= 0 && c.i0 < (int64_t)WB_MAX_TRACKS
                && c.i1 >= 0 && c.i1 < WB_MAX_INSERT_SLOTS)
                e->rtracks[c.i0].bypass[c.i1] = (int)c.f0;
            break;
        case WB_CMD_SET_INSERT_WET:
            if (e->rtracks && c.i0 >= 0 && c.i0 < (int64_t)WB_MAX_TRACKS
                && c.i1 >= 0 && c.i1 < WB_MAX_INSERT_SLOTS)
                e->rtracks[c.i0].wet[c.i1] = (float)c.f1;
            break;
        case WB_CMD_SET_SEND_LEVEL:
            if (e->rtracks && c.i0 >= 0 && c.i0 < (int64_t)WB_MAX_TRACKS
                && c.i1 >= 0 && c.i1 < (int64_t)WB_MAX_TRACKS)
                e->rtracks[c.i0].send[c.i1] = (float)c.f0;
            break;
        case WB_CMD_SET_SIDECHAIN:
            if (e->rtracks && c.i0 >= 0 && c.i0 < (int64_t)WB_MAX_TRACKS
                && c.i1 >= 0 && c.i1 < WB_MAX_INSERT_SLOTS)
                e->rtracks[c.i0].sidechainSrc[c.i1] = (int)c.i2;
            break;
        case WB_CMD_NOTE:
            if (e->rtracks && c.i0 >= 0 && c.i0 < (int64_t)WB_MAX_TRACKS) {
                wb_midifx_event ev;
                ev.pitch = (uint8_t)c.i1;
                ev.vel   = (uint8_t)(int)(c.f0 * 127.0);
                ev.on    = (int)(c.f0 > 0.0) ? 1 : 0;
                ev.tick  = 0;
                engine_route_note(e, (int)c.i0, &ev);
            }
            break;
        default: break;
        }
    }
}

/* ---- STAGE 1: schedule (spawn/retire notes from the timeline) -------- */
static void stage_schedule(wb_engine *e, uint32_t frames) {
    if (!e->session || !e->rtracks) return;
    for (uint32_t t = 0; t < e->session->track_count; t++) {
        wb_track_runtime *tr = &e->rtracks[t];
        if (!tr->active || !tr->voice || tr->kind != 0) continue;
        wb_track *tk = &e->session->tracks[t];
        if (tr->voice_unit_id && strcmp(tr->voice_unit_id, "fm") == 0)
            wb_transport_schedule_notes(tk, e->t.song_pos, frames,
                                        wb_fm_note, tr->voice);
        else if (tr->voice_unit_id && strcmp(tr->voice_unit_id, "drum") == 0)
            wb_transport_schedule_notes(tk, e->t.song_pos, frames,
                                        wb_drum_note, tr->voice);
        else
            wb_transport_schedule_notes(tk, e->t.song_pos, frames,
                                        wb_synth_note, tr->voice);
    }
}

/* ---- STAGE 1b: clock MIDI FX arpeggiators on a 1/16-note grid ---------- */
static void stage_midifx_tick(wb_engine *e, uint32_t frames) {
    if (!e->session || !e->rtracks) return;
    double spb = 60.0 / (e->t.bpm > 0 ? e->t.bpm : 120.0);   /* sec per beat */
    double sp16 = spb / 4.0;                                  /* sec per 1/16 */
    double ticks_d = (frames / (double)e->t.sample_rate) / sp16;/* 1/16 ticks this block */
    int ticks = (int)(ticks_d + 0.5);
    if (ticks < 1) ticks = 0;
    for (uint32_t t = 0; t < e->session->track_count; t++) {
        wb_track_runtime *tr = &e->rtracks[t];
        if (!tr->voice) continue;
        for (int s = 0; s < WB_MAX_INSERT_SLOTS; s++) {
            if (!tr->midifx[s]) continue;
            if (wb_midifx_get_type(tr->midifx[s]) != WB_MIDIFX_ARP) continue;
            wb_midifx_event out[16];
            int n = wb_midifx_tick(tr->midifx[s], ticks, out, 16);
            for (int i = 0; i < n; i++) {
                int pitch = out[i].pitch;
                int vel = out[i].on ? out[i].vel : 0;
                if (tr->voice_unit_id && strcmp(tr->voice_unit_id,"fm")==0)
                    wb_fm_note(tr->voice, pitch, (uint8_t)vel);
                else if (tr->voice_unit_id && strcmp(tr->voice_unit_id,"drum")==0)
                    wb_drum_note(tr->voice, pitch, (uint8_t)vel);
                else
                    wb_synth_note(tr->voice, pitch, (uint8_t)vel);
            }
        }
    }
}

/* ---- STAGE 2: instruments (render each track's voice to its buffer) -- */
static void stage_instruments(wb_engine *e, uint32_t frames) {
    if (!e->session || !e->rtracks) return;
    for (uint32_t t = 0; t < e->session->track_count; t++) {
        wb_track_runtime *tr = &e->rtracks[t];
        if (!tr->active) continue;
        memset(tr->bufL, 0, frames * sizeof(wb_sample));
        memset(tr->bufR, 0, frames * sizeof(wb_sample));
        if (tr->kind == 1) {
            /* audio track: render the active audio clip region into the block */
            wb_track *tk = &e->session->tracks[t];
            double pos = e->t.song_pos;
            for (uint32_t c = 0; c < tk->clip_count; c++) {
                wb_clip *cl = &tk->clips[c];
                if (!cl->audio_data || cl->audio_frames == 0) continue;
                double cl_end = cl->start + cl->length;
                /* skip clips that don't overlap this block */
                if (cl_end <= pos || cl->start >= pos + frames) continue;
                uint32_t ch = cl->audio_channels > 0 ? cl->audio_channels : 1;
                for (uint32_t i = 0; i < frames; i++) {
                    double sp = pos + i;
                    if (sp < cl->start || sp >= cl_end) continue;
                    double f = sp - cl->start;
                    uint32_t idx = (uint32_t)f;
                    if (idx >= cl->audio_frames) continue;
                    float vL = cl->audio_data[idx*ch];
                    float vR = ch > 1 ? cl->audio_data[idx*ch+1] : vL;
                    /* R022: clip (region) gain — pre-fader, before track vol */
                    float cg = cl->clip_gain > 0.0001f ? cl->clip_gain : 1.0f;
                    tr->bufL[i] += vL * cg;
                    tr->bufR[i] += vR * cg;
                }
            }
        } else if (tr->kind == 0 && tr->voice) {
            if (tr->voice_unit_id && strcmp(tr->voice_unit_id, "fm") == 0)
                wb_fm_render(tr->voice, tr->bufL, tr->bufR, frames);
            else if (tr->voice_unit_id && strcmp(tr->voice_unit_id, "drum") == 0)
                wb_drum_render(tr->voice, tr->bufL, tr->bufR, frames);
            else
                wb_synth_render_block(tr->voice, tr->bufL, tr->bufR, frames);
        }
    }
}

/* ---- STAGE 2.5: bus routing (sum routed tracks into their group bus) -- */
/* Runs AFTER stage_effects so each source's insert chain is already applied.
 * Each track routed to a bus (route >= 0) has its post-FX buffer added into
 * the bus's buffer, then the bus's own insert chain runs on the accumulated
 * signal. The bus is summed to master in stage_mix. */
static void stage_bus(wb_engine *e, uint32_t frames) {
    if (!e->session || !e->rtracks) return;
    for (uint32_t t = 0; t < e->session->track_count; t++) {
        wb_track_runtime *tr = &e->rtracks[t];
        if (!tr->active || tr->mute || tr->kind == 2) continue; /* skip buses */
        int r = tr->route;
        if (r < 0 || (uint32_t)r >= e->session->track_count) continue;
        wb_track_runtime *bus = &e->rtracks[r];
        if (bus->kind != 2 || !bus->active) continue;
        /* add this track's post-FX buffer into the bus */
        for (uint32_t i = 0; i < frames; i++) {
            bus->bufL[i] += tr->bufL[i];
            bus->bufR[i] += tr->bufR[i];
        }
    }
    /* run each bus's insert chain on its accumulated buffer.
     * per-slot bypass/wet honored (same contract as track inserts). */
    for (uint32_t t = 0; t < e->session->track_count; t++) {
        wb_track_runtime *tr = &e->rtracks[t];
        if (!tr->active || tr->kind != 2) continue;
        for (int s = 0; s < WB_MAX_INSERT_SLOTS; s++) {
            if (tr->bypass[s]) continue;
            float w = tr->wet[s];
            if (w <= 0.0f) continue;
            void *ins = tr->inserts[s];
            if (!ins) continue;
            const char *id = tr->insert_ids[s];
            const wb_unit *u = id ? wb_unit_find(id) : NULL;
            if (u && u->vt->process) {
                wb_sample dryL[WB_MAX_BLOCK], dryR[WB_MAX_BLOCK];
                if (w < 1.0f) {
                    memcpy(dryL, tr->bufL, frames * sizeof(wb_sample));
                    memcpy(dryR, tr->bufR, frames * sizeof(wb_sample));
                }
                u->vt->process(ins, tr->bufL, tr->bufR, frames);
                if (w < 1.0f) {
                    for (uint32_t i = 0; i < frames; i++) {
                        tr->bufL[i] = dryL[i] * (1.0f - w) + tr->bufL[i] * w;
                        tr->bufR[i] = dryR[i] * (1.0f - w) + tr->bufR[i] * w;
                    }
                }
            } else if (id && strncmp(id,"clap:",5)==0) {
                const wb_unit *cu = wb_unit_find("clap");
                if (cu && cu->vt->process) {
                    wb_sample dryL[WB_MAX_BLOCK], dryR[WB_MAX_BLOCK];
                    if (w < 1.0f) {
                        memcpy(dryL, tr->bufL, frames * sizeof(wb_sample));
                        memcpy(dryR, tr->bufR, frames * sizeof(wb_sample));
                    }
                    cu->vt->process(ins, tr->bufL, tr->bufR, frames);
                    if (w < 1.0f) {
                        for (uint32_t i = 0; i < frames; i++) {
                            tr->bufL[i] = dryL[i] * (1.0f - w) + tr->bufL[i] * w;
                            tr->bufR[i] = dryR[i] * (1.0f - w) + tr->bufR[i] * w;
                        }
                    }
                }
            } else if (id && strcmp(id,"comp")==0)   wb_comp_inplace_wet(ins, tr->bufL, tr->bufR, frames, w);
            else if (id && strcmp(id,"reverb")==0)   wb_reverb_inplace_wet(ins, tr->bufL, tr->bufR, frames, w);
            else if (id && strcmp(id,"delay")==0)    wb_delay_inplace_wet(ins, tr->bufL, tr->bufR, frames, w);
        }
    }
}

/* ---- STAGE 3: effects (run each track's insert chain) ---------------- */
static void stage_effects(wb_engine *e, uint32_t frames) {
    if (!e->session || !e->rtracks) return;
    /* aux send tap: after each track's FX, tap its post-FX buffer into any
     * destination tracks that have a non-zero send level from this source.
     * This runs BEFORE the FX chain so the send carries the dry signal.
     * (Post-FX sends: tap AFTER the chain — implemented below after FX.) */
    for (uint32_t t = 0; t < e->session->track_count; t++) {
        wb_track_runtime *tr = &e->rtracks[t];
        if (!tr->active || tr->kind == 2) continue;
        /* insert chain, in slot order. Each effect reads+writes the track
         * buffer in place. per-slot bypass/wet: if bypassed, skip; otherwise
         * apply the wet mix (0.0 = fully dry, 1.0 = fully processed). */
        for (int s = 0; s < WB_MAX_INSERT_SLOTS; s++) {
            /* sidechain: feed the source track's block into this slot's key input
             * (compressor sidechain). Must run before the comp slot processes. */
            int sc = tr->sidechainSrc[s];
            if (sc >= 0 && sc < (int)e->session->track_count && sc != (int)t) {
                const char *sid = tr->insert_ids[s];
                if (sid && strcmp(sid, "comp") == 0 && tr->inserts[s]) {
                    wb_track_runtime *src = &e->rtracks[sc];
                    if (src->active && src->bufL && src->bufR)
                        wb_comp_set_key(tr->inserts[s], src->bufL, src->bufR, frames);
                }
            }
            if (tr->bypass[s]) continue;
            float w = tr->wet[s];
            if (w <= 0.0f) continue; /* fully dry: no-op, keep buf untouched */
            void *ins = tr->inserts[s];
            if (!ins) continue;
            const char *id = tr->insert_ids[s];
            const wb_unit *u = id ? wb_unit_find(id) : NULL;
            if (u && u->vt->process) {
                wb_sample dryL[WB_MAX_BLOCK], dryR[WB_MAX_BLOCK];
                if (w < 1.0f) {
                    memcpy(dryL, tr->bufL, frames * sizeof(wb_sample));
                    memcpy(dryR, tr->bufR, frames * sizeof(wb_sample));
                }
                u->vt->process(ins, tr->bufL, tr->bufR, frames);
                if (w < 1.0f) {
                    for (uint32_t i = 0; i < frames; i++) {
                        tr->bufL[i] = dryL[i] * (1.0f - w) + tr->bufL[i] * w;
                        tr->bufR[i] = dryR[i] * (1.0f - w) + tr->bufR[i] * w;
                    }
                }
            } else if (id && strncmp(id,"clap:",5)==0) {
                const wb_unit *cu = wb_unit_find("clap");
                if (cu && cu->vt->process) {
                    wb_sample dryL[WB_MAX_BLOCK], dryR[WB_MAX_BLOCK];
                    if (w < 1.0f) {
                        memcpy(dryL, tr->bufL, frames * sizeof(wb_sample));
                        memcpy(dryR, tr->bufR, frames * sizeof(wb_sample));
                    }
                    cu->vt->process(ins, tr->bufL, tr->bufR, frames);
                    if (w < 1.0f) {
                        for (uint32_t i = 0; i < frames; i++) {
                            tr->bufL[i] = dryL[i] * (1.0f - w) + tr->bufL[i] * w;
                            tr->bufR[i] = dryR[i] * (1.0f - w) + tr->bufR[i] * w;
                        }
                    }
                }
            } else if (id && strcmp(id,"comp")==0)      wb_comp_inplace_wet(ins, tr->bufL, tr->bufR, frames, w);
            else if (id && strcmp(id,"reverb")==0)       wb_reverb_inplace_wet(ins, tr->bufL, tr->bufR, frames, w);
            else if (id && strcmp(id,"delay")==0)        wb_delay_inplace_wet(ins, tr->bufL, tr->bufR, frames, w);
            else if (id && strncmp(id,"vst3:",5)==0) {
                /* VST3 plugin: instance stored in the global slot map, keyed
                 * by track+slot. The "vst3:" prefix identifies the slot as
                 * holding a VST3 plugin; the plugin name follows the prefix. */
                const char *vname = id + 5;
                void *vinst = wb_vst3_slot_get((int)t, s);
                if (vinst && vname && *vname) {
                    wb_sample dryL[WB_MAX_BLOCK], dryR[WB_MAX_BLOCK];
                    if (w < 1.0f) {
                        memcpy(dryL, tr->bufL, frames * sizeof(wb_sample));
                        memcpy(dryR, tr->bufR, frames * sizeof(wb_sample));
                    }
                    int rc = wb_vst3_process(vinst, tr->bufL, tr->bufR,
                                             tr->bufL, tr->bufR, frames);
                    (void)rc;
                    if (w < 1.0f) {
                        for (uint32_t i = 0; i < frames; i++) {
                            tr->bufL[i] = dryL[i] * (1.0f - w) + tr->bufL[i] * w;
                            tr->bufR[i] = dryR[i] * (1.0f - w) + tr->bufR[i] * w;
                        }
                    }
                }
            }
        }
        /* post-FX aux send tap: send this track's post-FX buffer to any
         * destination that has send[dst] > 0 from this source track. */
        for (uint32_t dst = 0; dst < e->session->track_count; dst++) {
            float sl = tr->send[dst];
            if (sl <= 0.0f) continue;
            wb_track_runtime *dt = &e->rtracks[dst];
            if (!dt->active) continue;
            for (uint32_t i = 0; i < frames; i++) {
                dt->bufL[i] += tr->bufL[i] * sl;
                dt->bufR[i] += tr->bufR[i] * sl;
            }
        }
        /* R024: live meter — measure this track's actual post-FX level before
         * it is summed to master (so the meter shows real signal, not fader).
         * A muted or solo-excluded track is silent to the listener, so its
         * meter reads zero too (matches what the user hears). */
        {
            int any_solo = 0;
            for (uint32_t tt = 0; tt < e->session->track_count; tt++)
                if (e->rtracks[tt].solo) any_solo = 1;
            wb_track *st = &e->session->tracks[t];
            float pk = 0.0f, sum = 0.0f;
            if (!st->mute && (!any_solo || st->solo)) {
                for (uint32_t i = 0; i < frames; i++) {
                    float vL = tr->bufL[i] < 0 ? -tr->bufL[i] : tr->bufL[i];
                    float vR = tr->bufR[i] < 0 ? -tr->bufR[i] : tr->bufR[i];
                    float v = vL > vR ? vL : vR;
                    if (v > pk) pk = v;
                    sum += v * v;
                }
            }
            st->meter_peak = pk;
            st->meter_rms  = (float)sqrtf(sum / (double)frames);
        }
    }
}

/* ---- master mix + sum ------------------------------------------------ */
static void stage_mix(wb_engine *e, uint32_t n, wb_sample *out) {
    int any_solo = 0;
    if (e->session && e->rtracks)
        for (uint32_t t = 0; t < e->session->track_count; t++)
            if (e->rtracks[t].solo) any_solo = 1;

    if (e->session && e->rtracks) {
        for (uint32_t t = 0; t < e->session->track_count; t++) {
            wb_track_runtime *tr = &e->rtracks[t];
            if (!tr->active || tr->mute) continue;
            if (any_solo && !tr->solo) continue;
            /* a track routed to a bus is summed into that bus already (its
             * signal reaches the master only through the bus); skip it here. */
            if (tr->kind != 2 && tr->route >= 0) continue;
            float l = (float)(1.0 - (tr->pan > 0 ? tr->pan : 0));
            float r = (float)(1.0 - (tr->pan < 0 ? -tr->pan : 0));
            float g = tr->volume;
            for (uint32_t i = 0; i < n; i++) {
                out[2*i]   += tr->bufL[i] * g * l;
                out[2*i+1] += tr->bufR[i] * g * r;
            }
        }
    }
    /* apply master volume automation / fader */
    float mv = e->master_volume;
    for (uint32_t i = 0; i < (n * 2); i++) out[i] *= mv;
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
    e->master_volume = 1.0f;
    if (pthread_mutex_init(&e->process_lock, NULL) == 0)
        e->lock_initialized = 1;
    wb_unit_ensure_all();   /* register built-in FX + instruments */
    e->mod = wb_mod_matrix_create();
    return e;
}

void wb_engine_destroy(wb_engine *e) {
    if (!e) return;
    if (e->rtracks) {
        for (int i = 0; i < (int)WB_MAX_TRACKS; i++) {
            const char *vid = e->rtracks[i].insert_ids[0];
            if (e->rtracks[i].voice) {
                const wb_unit *v = vid ? wb_unit_find(vid) : NULL;
                if (v && v->vt->destroy) v->vt->destroy(e->rtracks[i].voice);
                else wb_synth_destroy(e->rtracks[i].voice); /* legacy fallback */
            }
            for (int s = 0; s < WB_MAX_INSERT_SLOTS; s++) {
                if (e->rtracks[i].inserts[s]) {
                    const char *id = e->rtracks[i].insert_ids[s];
                    const wb_unit *u = id ? wb_unit_find(id) : NULL;
                    if (u) u->vt->destroy(e->rtracks[i].inserts[s]);
                    else if (id && strcmp(id,"comp") ==0)  wb_comp_destroy(e->rtracks[i].inserts[s]);
                    else if (id && strcmp(id,"reverb")==0) wb_reverb_destroy(e->rtracks[i].inserts[s]);
                    else if (id && strcmp(id,"delay") ==0) wb_delay_destroy(e->rtracks[i].inserts[s]);
                    else free(e->rtracks[i].inserts[s]); /* unknown: best-effort */
                }
            }
            free(e->rtracks[i].bufL);
            free(e->rtracks[i].bufR);
        }
        free(e->rtracks);
    }
    if (e->lock_initialized) pthread_mutex_destroy(&e->process_lock);
    for (uint32_t t = 0; t < WB_MAX_TRACKS; t++)
        if (e->recorders[t]) wb_recorder_destroy(e->recorders[t]);
    free(e->accL);
    free(e->accR);
    if (e->mod) wb_mod_matrix_destroy(e->mod);
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
            wb_track_runtime *tr = &e->rtracks[i];
            if (tr->voice) {
                if (tr->voice_unit_id && strcmp(tr->voice_unit_id,"fm")==0) wb_fm_destroy(tr->voice);
                else if (tr->voice_unit_id && strcmp(tr->voice_unit_id,"drum")==0) wb_drum_destroy(tr->voice);
                else wb_synth_destroy(tr->voice);
            }
            for (int s = 0; s < WB_MAX_INSERT_SLOTS; s++) {
                if (tr->inserts[s]) {
                    const wb_unit *u = tr->insert_ids[s] ? wb_unit_find(tr->insert_ids[s]) : NULL;
                    if (u) u->vt->destroy(tr->inserts[s]);
                    else if (tr->insert_ids[s] && strcmp(tr->insert_ids[s],"comp")==0)   wb_comp_destroy(tr->inserts[s]);
                    else if (tr->insert_ids[s] && strcmp(tr->insert_ids[s],"reverb")==0) wb_reverb_destroy(tr->inserts[s]);
                    else if (tr->insert_ids[s] && strcmp(tr->insert_ids[s],"delay")==0)   wb_delay_destroy(tr->inserts[s]);
                }
            }
            for (int s = 0; s < WB_MAX_INSERT_SLOTS; s++)
                if (tr->midifx[s]) { wb_midifx_destroy(tr->midifx[s]); tr->midifx[s] = NULL; }
            free(tr->bufL); free(tr->bufR);
            memset(tr, 0, sizeof(*tr));
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
        tr->route = s->tracks[i].route;
        tr->volume = s->tracks[i].volume;
        tr->pan = s->tracks[i].pan;
        tr->mute = s->tracks[i].mute;
        tr->solo = s->tracks[i].solo;
        tr->buf_cap = WB_MAX_BLOCK;
        tr->bufL = malloc(WB_MAX_BLOCK * sizeof(wb_sample));
        tr->bufR = malloc(WB_MAX_BLOCK * sizeof(wb_sample));
        if (s->tracks[i].kind == 0) {
            /* first insert slot determines the instrument (default: synth) */
            const char *vuid = s->tracks[i].inserts[0].id;
            tr->voice_unit_id = vuid && vuid[0] ? vuid : "synth";
            tr->insert_ids[0] = vuid && vuid[0] ? vuid : "synth";
            if (tr->voice_unit_id && strcmp(tr->voice_unit_id,"fm")==0)
                tr->voice = wb_fm_create(WB_SAMPLE_RATE);
            else if (tr->voice_unit_id && strcmp(tr->voice_unit_id,"drum")==0)
                tr->voice = wb_drum_create(WB_SAMPLE_RATE);
            else
                tr->voice = wb_synth_create(WB_SAMPLE_RATE);
        }
        /* build insert chain: slot 0 is the instrument id, slots 1..N are FX.
         * The instrument is rendered separately in stage_instruments, so we
         * only create FX instances for slots >= 1. */
        for (int slot = 1; slot < WB_MAX_INSERT_SLOTS; slot++) {
            const char *id = s->tracks[i].inserts[slot].id;
            if (!id || !id[0]) continue;
            tr->insert_ids[slot] = id;
            tr->wet[slot] = 1.0f;  /* newly-created non-empty FX slots run fully wet by default */
            /* CLAP plugin slots use "clap:<descriptor_id>"; instantiate via
             * the bound host (falls back to the generic unit registry). */
            if (e->clap_host && strncmp(id, "clap:", 5) == 0) {
                tr->inserts[slot] = wb_unit_clap_create(e->clap_host, id, WB_SAMPLE_RATE);
                if (!tr->inserts[slot]) continue;
                continue;
            }
            const wb_unit *u = wb_unit_find(id);
            if (u) tr->inserts[slot] = u->vt->create(WB_SAMPLE_RATE);
            else if (strcmp(id,"comp") == 0)   tr->inserts[slot] = wb_comp_create(WB_SAMPLE_RATE);
            else if (strcmp(id,"reverb") == 0) tr->inserts[slot] = wb_reverb_create(WB_SAMPLE_RATE);
            else if (strcmp(id,"delay") == 0)  tr->inserts[slot] = wb_delay_create(WB_SAMPLE_RATE);
        }
        /* sync per-slot sidechain routing from session -> runtime (-1 = none) */
        for (int slot = 0; slot < WB_MAX_INSERT_SLOTS; slot++)
            tr->sidechainSrc[slot] = s->tracks[i].sidechain[slot];
    }
}

wb_session *wb_engine_get_session(wb_engine *e) { return e->session; }

void wb_engine_play(wb_engine *e) { wb_cmd c = { .type = WB_CMD_PLAY }; wb_cmd_push(&e->queue, c); }
void wb_engine_stop(wb_engine *e) { wb_cmd c = { .type = WB_CMD_STOP }; wb_cmd_push(&e->queue, c); }
void wb_engine_seek(wb_engine *e, double p) { wb_cmd c = { .type = WB_CMD_SEEK, .f0 = p }; wb_cmd_push(&e->queue, c); }
void wb_engine_set_bpm(wb_engine *e, double bpm) { wb_cmd c = { .type = WB_CMD_SET_BPM, .f0 = bpm }; wb_cmd_push(&e->queue, c); }
void wb_engine_set_clap_host(wb_engine *e, struct wb_clap_host *h) { if (e) e->clap_host = h; }
void wb_engine_get_transport(wb_engine *e, wb_transport *out) { if (out) *out = e->t; }
void wb_engine_set_track_volume(wb_engine *e, int track, float vol) {
    wb_cmd c = { .type = WB_CMD_SET_TRACK_VOL, .i0 = track, .f0 = vol };
    wb_cmd_push(&e->queue, c);
}
void wb_engine_note(wb_engine *e, int track, uint8_t pitch, uint8_t vel) {
    /* mirror into any armed recorder for this track (RT-safe) */
    if (e && track >= 0 && track < (int)WB_MAX_TRACKS && e->recorders[track]) {
        wb_recorder_midi_event(e->recorders[track], (int)pitch, (int)vel,
                               e->t.song_pos);
    }
    wb_cmd c = { .type = WB_CMD_NOTE, .i0 = track, .i1 = pitch, .f0 = vel };
    wb_cmd_push(&e->queue, c);
}

void wb_engine_record(wb_engine *e, int track, int clip_idx, int on, int overdub) {
    if (!e || track < 0 || track >= (int)WB_MAX_TRACKS) return;
    if (on) {
        wb_track *tk = e->session && track < (int)e->session->track_count
                       ? &e->session->tracks[track] : NULL;
        if (!tk || clip_idx < 0 || clip_idx >= (int)tk->clip_count) return;
        wb_recorder *r = wb_recorder_create(tk, clip_idx);
        if (!r) return;
        wb_recorder_set_overdub(r, overdub);
        e->recorders[track] = r;
    } else {
        if (e->recorders[track]) {
            wb_recorder_destroy(e->recorders[track]);
            e->recorders[track] = NULL;
        }
    }
}

/* ---- VST3 slot map -------------------------------------------------------
 * Per (track, slot) → VST3 plugin instance pointer. Tracked across render
 * so the engine can locate the right VST3 instance for process + param ops.
 * Keyed by (track index, slot index); one slot may hold at most one VST3
 * instance at a time. Insertion/destruction of VST3 plugins goes through
 * wb_vst3_slot_set / wb_vst3_slot_clear.
 */

static void *wb_vst3_slot_map[WB_MAX_TRACKS][WB_MAX_INSERT_SLOTS];

void *wb_vst3_slot_get(int track, int slot) {
    if (track < 0 || slot < 0 || track >= (int)WB_MAX_TRACKS || slot >= WB_MAX_INSERT_SLOTS)
        return NULL;
    return wb_vst3_slot_map[track][slot];
}

void wb_vst3_slot_set(int track, int slot, void *inst) {
    if (track < 0 || slot < 0 || track >= (int)WB_MAX_TRACKS || slot >= WB_MAX_INSERT_SLOTS)
        return;
    if (wb_vst3_slot_map[track][slot]) {
        wb_vst3_destroy(wb_vst3_slot_map[track][slot]);
    }
    wb_vst3_slot_map[track][slot] = inst;
}

void wb_vst3_slot_clear(int track, int slot) {
    if (track < 0 || slot < 0 || track >= (int)WB_MAX_TRACKS || slot >= WB_MAX_INSERT_SLOTS)
        return;
    if (wb_vst3_slot_map[track][slot]) {
        wb_vst3_destroy(wb_vst3_slot_map[track][slot]);
        wb_vst3_slot_map[track][slot] = NULL;
    }
}

/* ---- end VST3 slot map ------------------------------------------------- */
void wb_unit_set_param(const char *id, void *ins, const char *pname, float v01) {
    (void)id;
    if (!ins || !pname) return;
    const wb_unit *u = wb_unit_find(id);
    if (u && u->vt->set_param) {
        u->vt->set_param(ins, pname, v01);
    }
}

void wb_engine_set_insert_param(wb_engine *e, int track, int slot, int param, float value) {
    if (!e) return;
    if (track < 0 || slot < 0 || track >= (int)WB_MAX_TRACKS || slot >= WB_MAX_INSERT_SLOTS)
        return;
    const char *id = e->rtracks[track].insert_ids[slot];
    if (id && strncmp(id, "vst3:", 5) == 0) {
        /* VST3 plugin param: route to the VST3 host by param index.
         * param is the VST3 parameter index (0..getParameterCount-1). */
        void *vinst = wb_vst3_slot_get(track, slot);
        if (vinst) {
            wb_vst3_set_param(vinst, param, value);
        }
        return;
    }
    if (id && strncmp(id, "clap:", 5) == 0) {
        /* CLAP plugin param: route via the CLAP host (future work). */
        return;
    }
    /* built-in units: use the vtable's set_param */
    const wb_unit *u = id ? wb_unit_find(id) : NULL;
    if (u && u->vt->set_param) {
        wb_unit_set_param(id, e->rtracks[track].inserts[slot], id, value);
    }
}

wb_mod_matrix *wb_engine_get_mod_matrix(wb_engine *e) {
    return e ? e->mod : NULL;
}

void wb_engine_set_insert_bypass(wb_engine *e, int track, int slot, int on) {
    if (!e) return;
    wb_cmd c = { .type = WB_CMD_SET_INSERT_BYPASS, .i0 = track, .i1 = slot, .f0 = on ? 1.0 : 0.0 };
    wb_cmd_push(&e->queue, c);
}

void wb_engine_set_insert_wet(wb_engine *e, int track, int slot, float wet) {
    if (!e) return;
    wb_cmd c = { .type = WB_CMD_SET_INSERT_WET, .i0 = track, .i1 = slot, .f1 = wet };
    wb_cmd_push(&e->queue, c);
}

void wb_engine_set_send_level(wb_engine *e, int src_track, int dst_track, float level) {
    if (!e || src_track < 0 || dst_track < 0
        || src_track >= (int)WB_MAX_TRACKS || dst_track >= (int)WB_MAX_TRACKS)
        return;
    /* update session model (persisted in .wbus) */
    if (e->session && src_track < (int)e->session->track_count
        && dst_track < (int)e->session->track_count)
        e->session->tracks[src_track].send[dst_track] = level;
    /* push to RT cmd queue so the next render block sees it */
    wb_cmd c = { .type = WB_CMD_SET_SEND_LEVEL, .i0 = src_track, .i1 = dst_track, .f0 = level };
    wb_cmd_push(&e->queue, c);
}

void wb_engine_set_insert_sidechain(wb_engine *e, int track, int slot, int src_track) {
    if (!e || track < 0 || slot < 0 || src_track < -1
        || track >= (int)WB_MAX_TRACKS || slot >= WB_MAX_INSERT_SLOTS
        || src_track >= (int)WB_MAX_TRACKS)
        return;
    if (e->session && track < (int)e->session->track_count)
        e->session->tracks[track].sidechain[slot] = src_track;
    wb_cmd c = { .type = WB_CMD_SET_SIDECHAIN, .i0 = track, .i1 = slot, .i2 = src_track };
    wb_cmd_push(&e->queue, c);
}

int wb_engine_set_midifx(wb_engine *e, int track, int slot, wb_midifx_type type) {
    if (!e || track < 0 || slot < 0 || track >= (int)WB_MAX_TRACKS || slot >= WB_MAX_INSERT_SLOTS)
        return -1;
    if (track >= (int)e->session->track_count) return -1;
    /* destroy any existing unit in this slot (RT-safe: swap under edit lock) */
    wb_engine_begin_edit(e);
    if (e->rtracks[track].midifx[slot])
        wb_midifx_destroy(e->rtracks[track].midifx[slot]);
    e->rtracks[track].midifx[slot] = (type == WB_MIDIFX_NONE) ? NULL : wb_midifx_create(type);
    wb_engine_end_edit(e);
    return 0;
}

void wb_engine_set_midifx_param(wb_engine *e, int track, int slot, int param, float value) {
    if (!e || track < 0 || slot < 0 || track >= (int)WB_MAX_TRACKS || slot >= WB_MAX_INSERT_SLOTS)
        return;
    if (e->rtracks[track].midifx[slot])
        wb_midifx_set_param(e->rtracks[track].midifx[slot], param, value);
}

void wb_engine_begin_edit(wb_engine *e) {
    if (e && e->lock_initialized) pthread_mutex_lock(&e->process_lock);
}
void wb_engine_end_edit(wb_engine *e) {
    if (e && e->lock_initialized) pthread_mutex_unlock(&e->process_lock);
}

uint64_t wb_engine_xruns(wb_engine *e) { return e ? e->xruns : 0; }

/* ---- STAGE: evaluate automation lanes against the current song position -- */
static void stage_automation(wb_engine *e, uint32_t n) {
    (void)n;
    wb_session *s = e->session;
    if (!s || !s->automation_count) return;
    double pos = (double)e->t.song_pos;
    for (uint32_t l = 0; l < s->automation_count; l++) {
        wb_automation_lane *al = s->automation[l];
        if (!al->point_count) continue;
        double val = wb_automation_value_at(al, pos, -1.0);
        if (val < 0.0) continue; /* no sample fell in any segment */
        if (al->target < 0) {
            e->master_volume = (float)val; /* master volume automation */
            continue;
        }
        if ((uint32_t)al->target >= s->track_count) continue;
        wb_track_runtime *tr = &e->rtracks[al->target];
        if (!strcmp(al->param, "volume"))
            tr->volume = (float)val;
        else if (!strcmp(al->param, "pan"))
            tr->pan = (float)(val * 2.0 - 1.0); /* 0..1 -> -1..1 */
    }
}

/* ---- STAGE: evaluate the modulation matrix and push to param destinations -- */
static void wb_mod_setter_cb(void *ctx, int track, int slot, int param, float value01) {
    wb_engine *e = (wb_engine *)ctx;
    wb_engine_set_insert_param(e, track, slot, param, value01);
}

static void stage_modulation(wb_engine *e, uint32_t n) {
    if (!e->mod) return;
    wb_mod_matrix_eval(e->mod, n, (float)e->t.sample_rate, wb_mod_setter_cb, e);
}

uint32_t wb_engine_render(wb_engine *e, wb_sample *out, uint32_t n) {
    if (!e || !out || n == 0) return 0;
    if (n > e->acc_cap) return 0;

    /* R002: try-lock the process mutex. If a non-RT thread is mid-edit,
     * do NOT block the audio thread — count an Xrun, silence, return. */
    if (e->lock_initialized && pthread_mutex_trylock(&e->process_lock) != 0) {
        e->xruns++;
        memset(out, 0, n * 2 * sizeof(wb_sample));
        return n;
    }

    engine_process_cmds(e);
    memset(out, 0, n * 2 * sizeof(wb_sample));

    /* staged pipeline (R002) */
    stage_schedule(e, n);
    stage_midifx_tick(e, n);
    stage_automation(e, n);
    stage_modulation(e, n);
    stage_instruments(e, n);
    stage_effects(e, n);
    stage_bus(e, n);
    /* flush RT-captured MIDI into authored clips (non-RT realloc path).
     * The block covers [song_pos, song_pos+n); held notes at block end are
     * extended to song_pos+n until a note-off resolves them. */
    double block_end = (double)(e->t.song_pos + n);
    for (uint32_t t = 0; t < WB_MAX_TRACKS; t++)
        if (e->recorders[t])
            wb_recorder_flush(e->recorders[t], block_end);
    stage_mix(e, n, out);

    /* double-buffered advance: transport moves by the rendered block */
    if (e->t.playing) {
        e->t.song_pos += n;
        if (e->t.loop_on && (e->t.loop_end - e->t.loop_start) > 0) {
            if (e->t.song_pos >= e->t.loop_end)
                e->t.song_pos = e->t.loop_start +
                    fmod(e->t.song_pos - e->t.loop_start,
                         e->t.loop_end - e->t.loop_start);
        }
    }

    if (e->lock_initialized) pthread_mutex_unlock(&e->process_lock);
    return n;
}

float wb_engine_cpu_load(wb_engine *e) { return e ? e->cpu_load : 0; }

int wb_engine_render_session(wb_engine *e, wb_session *s, wb_sample **out, uint32_t *frames) {
    (void)e; /* we render into a private engine; keep the signature for API compat */
    if (!s || s->length <= 0) return -1;
    wb_engine *tmp = wb_engine_create();
    wb_engine_set_session(tmp, s);
    wb_engine_seek(tmp, 0);
    tmp->t.playing = 1;

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
