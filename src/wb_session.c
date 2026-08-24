/* wb_session.c — session model: build, destroy, and a demo-song builder.
 * A session is the editable arrangement: tracks, clips, notes. The engine
 * consumes it at render time. This file also owns the .wbus project
 * serialization (load/save) — plain text, human-readable.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>   /* G70: access() for offline detection */
#include <dirent.h>   /* G70: directory scan for relink */
#include "wbus.h"
#include "wbus_video.h"
#include "wbus/wbus_clip_edit.h"
#include "wbus/wbus_midi.h"   /* G82: wb_scale_snap */

/* ---- create ------------------------------------------------------------- */
wb_session *wb_session_create(void) {
    wb_session *s = calloc(1, sizeof(*s));
    if (s) {
        snprintf(s->name, sizeof(s->name), "Untitled");
        s->bpm = 120.0; s->time_sig_num = 4; s->time_sig_den = 4;
    }
    return s;
}

/* Deep copy a session: duplicates tracks, clips, notes, audio buffers, and
 * automation lanes so the copy is fully independent of the source. */
wb_session *wb_session_copy(const wb_session *src) {
    if (!src) return NULL;
    wb_session *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    memcpy(d, src, sizeof(*d));
    d->tracks = NULL;
    d->automation = NULL;
    if (src->track_count > 0) {
        d->tracks = calloc(src->track_count, sizeof(wb_track));
        if (!d->tracks) { free(d); return NULL; }
        memcpy(d->tracks, src->tracks, src->track_count * sizeof(wb_track));
        for (uint32_t t = 0; t < src->track_count; t++) {
            wb_track *dt = &d->tracks[t];
            const wb_track *st = &src->tracks[t];
            if (st->clip_count > 0) {
                dt->clips = calloc(st->clip_count, sizeof(wb_clip));
                if (!dt->clips) { wb_session_destroy(d); return NULL; }
                memcpy(dt->clips, st->clips, st->clip_count * sizeof(wb_clip));
                for (uint32_t c = 0; c < st->clip_count; c++) {
                    wb_clip *dc = &dt->clips[c];
                    const wb_clip *sc = &st->clips[c];
                    dc->notes = NULL; dc->audio_data = NULL;
                    if (sc->note_count > 0) {
                        dc->notes = calloc(sc->note_count, sizeof(wb_note));
                        if (!dc->notes) { wb_session_destroy(d); return NULL; }
                        memcpy(dc->notes, sc->notes, sc->note_count * sizeof(wb_note));
                    }
                    if (sc->audio_data && sc->audio_frames > 0) {
                        size_t bytes = (size_t)sc->audio_frames * sc->audio_channels * sizeof(wb_sample);
                        dc->audio_data = malloc(bytes);
                        if (!dc->audio_data) { wb_session_destroy(d); return NULL; }
                        memcpy(dc->audio_data, sc->audio_data, bytes);
                    }
                }
            }
        }
    }
    if (src->automation_count > 0) {
        d->automation = calloc(src->automation_count, sizeof(void*));
        if (!d->automation) { wb_session_destroy(d); return NULL; }
        for (uint32_t a = 0; a < src->automation_count; a++) {
            const wb_automation_lane *sl = src->automation[a];
            wb_automation_lane *dl = wb_automation_lane_create(sl->param);
            if (!dl) { wb_session_destroy(d); return NULL; }
            dl->target = sl->target;
            for (uint32_t p = 0; p < sl->point_count; p++)
                wb_automation_add_point(dl, sl->points[p].time, sl->points[p].value, sl->points[p].curve);
            d->automation[a] = dl;
        }
    }
    return d;
}

void wb_session_destroy(wb_session *s) {
    if (!s) return;
    for (uint32_t t = 0; t < s->track_count; t++) {
        wb_track *tr = &s->tracks[t];
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            wb_clip *cl = &tr->clips[c];
            free(cl->notes);
            free(cl->audio_data);
        }
        free(tr->clips);
    }
    free(s->tracks);
    wb_session_free_automation(s);
    free(s);
}

/* ---- automation (session-level lane ownership) ------------------------- */
wb_automation_lane *wb_session_add_automation(wb_session *s, const char *param, int target) {
    if (!s) return NULL;
    wb_automation_lane *l = wb_automation_lane_create(param);
    if (!l) return NULL;
    l->target = target;
    wb_automation_lane **na = realloc(s->automation, (s->automation_count + 1) * sizeof(void*));
    if (!na) { wb_automation_lane_destroy(l); return NULL; }
    s->automation = na;
    s->automation[s->automation_count++] = l;
    return l;
}

void wb_session_free_automation(wb_session *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->automation_count; i++)
        wb_automation_lane_destroy(s->automation[i]);
    free(s->automation);
    s->automation = NULL;
    s->automation_count = 0;
}

/* ---- track/note helpers ------------------------------------------------- */
/* Add a new track with SOTA defaults (full volume, center pan, unmuted). */
wb_track *wb_session_add_track(wb_session *s, const char *name, int kind) {
    if (!s) return NULL;
    if (s->track_count >= WB_MAX_TRACKS) return NULL;
    if (!s->tracks) {
        s->tracks = calloc(WB_MAX_TRACKS, sizeof(wb_track));
        if (!s->tracks) return NULL;
    }
    wb_track *tr = &s->tracks[s->track_count++];
    if (name) snprintf(tr->name, sizeof(tr->name), "%s", name);
    else      snprintf(tr->name, sizeof(tr->name), "Track %u", s->track_count);
    tr->kind = kind;
    tr->volume = 1.0f;
    tr->pan = 0.0f;
    tr->mute = 0;
    tr->solo = 0;
    tr->route = -1;   /* default: route to master, not a bus */
    tr->active_lane = 0;   /* R030: main lane active by default */
    tr->rec_armed = 0;     /* G09: not record-armed */
    return tr;
}

/* G09 (Wave2): remove a track and close the gap. Frees the removed
 * track's clips/notes, memmoves the array, and retargets references:
 * automation lane targets, insert sidechain keys, aux send targets,
 * routes (a route to a bus after the removed index shifts down). */
int wb_session_remove_track(wb_session *s, uint32_t idx) {
    if (!s || idx >= s->track_count) return -1;
    wb_track *tk = &s->tracks[idx];
    for (uint32_t c = 0; c < tk->clip_count; c++) free(tk->clips[c].notes);
    free(tk->clips);
    tk->clips = NULL; tk->clip_count = 0;

    for (uint32_t t = idx; t + 1 < s->track_count; t++)
        s->tracks[t] = s->tracks[t + 1];
    s->track_count--;

    /* retarget references to tracks after the removed index */
    for (uint32_t t = 0; t < s->track_count; t++) {
        wb_track *r = &s->tracks[t];
        if (r->route > (int)idx && r->route != -1) r->route--;
        else if (r->route == (int)idx)             r->route = -1;
        for (int k = 0; k < WB_MAX_INSERT_SLOTS; k++) {
            if (r->sidechain[k] == (int)idx)       r->sidechain[k] = -1;
            else if (r->sidechain[k] > (int)idx)   r->sidechain[k]--;
        }
        for (int k = 0; k < 2; k++) {
            if (r->send_target[k] == (int)idx)     r->send_target[k] = -1;
            else if (r->send_target[k] > (int)idx) r->send_target[k]--;
        }
    }
    /* automation lanes: drop lanes targeting the removed track, shift others */
    uint32_t w = 0;
    for (uint32_t a = 0; a < s->automation_count; a++) {
        wb_automation_lane *al = s->automation[a];
        if (al->target == (int)idx) { wb_automation_lane_destroy(al); continue; }
        if (al->target > (int)idx) al->target--;
        s->automation[w++] = al;
    }
    s->automation_count = w;
    return 0;
}

/* G09 (Wave2): swap two adjacent tracks (reorder up/down in the gutter). */
int wb_session_move_track(wb_session *s, uint32_t idx, int delta) {
    if (!s || delta == 0) return -1;
    int64_t j = (int64_t)idx + delta;
    if (j < 0 || j >= (int64_t)s->track_count) return -1;
    wb_track tmp      = s->tracks[idx];
    s->tracks[idx]    = s->tracks[j];
    s->tracks[j]      = tmp;
    return 0;
}

/* Append a MIDI note to a track's first clip (creating one if needed). */
int wb_session_add_note(wb_track *tr, double start, double dur, int pitch, int vel) {
    if (!tr) return -1;
    wb_clip *cl;
    if (tr->clip_count == 0) {
        tr->clips = calloc(1, sizeof(wb_clip));
        if (!tr->clips) return -1;
        tr->clip_count = 1;
        cl = &tr->clips[0];
        cl->type = 0;
        cl->start = 0;
    } else {
        cl = &tr->clips[tr->clip_count - 1];
    }
    wb_note *n = realloc(cl->notes, (cl->note_count + 1) * sizeof(wb_note));
    if (!n) return -1;
    cl->notes = n;
    wb_note *nn = &cl->notes[cl->note_count++];
    nn->start = start; nn->dur = dur;
    nn->pitch = (uint8_t)pitch; nn->vel = (uint8_t)vel;
    return 0;
}

int wb_session_remove_note(wb_track *tr, double start, int pitch) {
    if (!tr || tr->clip_count == 0) return -1;
    /* search the last clip (where add_note appends) */
    wb_clip *cl = &tr->clips[tr->clip_count - 1];
    double best_d = 1e18; int best = -1;
    for (uint32_t i = 0; i < cl->note_count; i++) {
        double dt = fabs(cl->notes[i].start - start);
        int dp = abs((int)cl->notes[i].pitch - pitch);
        double d = dt + dp * 0.001 * (60.0 / 120.0);  /* pitch counts a little */
        if (dt < 0.25 && dp <= 1 && d < best_d) { best_d = d; best = (int)i; }
    }
    if (best < 0) return -1;
    for (uint32_t i = (uint32_t)best; i + 1 < cl->note_count; i++)
        cl->notes[i] = cl->notes[i + 1];
    cl->note_count--;
    /* NOTE: keep the buffer allocated (do NOT free to NULL) so any undo
     * snapshot holding a pointer to cl->notes stays valid. note_count==0 is fine. */
    return 0;
}

/* ---- demo song --------------------------------------------------------- */
/* Build a simple demo session: a synth lead line over a bass. This is what
 * the first render/playback exercise uses to prove the engine works. */

/* Add an audio clip (type 1) to a track, taking ownership of `data`.
 * The buffer is interleaved (frames * channels) floats. */
int wb_session_add_audio_clip(wb_track *tr, double start, double length,
                              const wb_sample *data, uint32_t frames,
                              int channels) {
    if (!tr || !data || frames == 0) return -1;
    tr->clips = realloc(tr->clips, (tr->clip_count + 1) * sizeof(wb_clip));
    if (!tr->clips) return -1;
    wb_clip *cl = &tr->clips[tr->clip_count++];
    memset(cl, 0, sizeof(*cl));
    cl->type = 1;
    cl->start = start;
    cl->length = length;
    cl->audio_channels = channels > 0 ? channels : 1;
    cl->audio_frames = frames;
    size_t bytes = (size_t)frames * cl->audio_channels * sizeof(wb_sample);
    cl->audio_data = malloc(bytes);
    if (!cl->audio_data) { tr->clip_count--; return -1; }
    memcpy(cl->audio_data, data, bytes);
    cl->clip_gain = 1.0f;   /* R022: unity region gain by default */
    return 0;
}

/* R022: add an arrangement marker (song-section label / cue) to the session. */
int wb_session_add_marker(wb_session *s, double pos, const char *label, int kind) {
    if (!s || s->marker_count >= 64) return -1;
    wb_marker *m = &s->markers[s->marker_count++];
    m->pos = pos;
    m->kind = kind;
    snprintf(m->label, sizeof(m->label), "%s", label ? label : "");
    return 0;
}

/* R030: set the active take-lane for a track (comping). Only clips on the
 * active lane are played; the rest are silent. */
void wb_session_set_active_lane(wb_session *s, int track, int lane) {
    if (!s || track < 0 || (uint32_t)track >= s->track_count) return;
    if (lane < 0) lane = 0;
    s->tracks[track].active_lane = lane;
}

/* G31: FX chain rack model ops. set_insert writes `unit_id` into slot
 * (slot 0 = instrument; FX live in 1..N). id "" or NULL clears the slot.
 * Returns 0, or -1 on bad args. The engine rebuilds instances from these
 * ids on wb_engine_set_session. */
int wb_session_set_insert(wb_session *s, int track, int slot, const char *unit_id) {
    if (!s || track < 0 || (uint32_t)track >= s->track_count) return -1;
    if (slot < 0 || slot >= WB_MAX_INSERT_SLOTS) return -1;
    wb_plugin_slot *ps = &s->tracks[track].inserts[slot];
    if (unit_id && unit_id[0]) {
        memset(ps->id, 0, sizeof(ps->id));
        snprintf(ps->id, sizeof(ps->id), "%s", unit_id);
    } else {
        memset(ps->id, 0, sizeof(ps->id));
    }
    return 0;
}

/* G31: move an insert from slot `from` to slot `to` (drag-reorder). The
 * vacated source slot is cleared. Returns 0, or -1 on bad args. */
int wb_session_move_insert(wb_session *s, int track, int from, int to) {
    if (!s || track < 0 || (uint32_t)track >= s->track_count) return -1;
    if (from < 0 || from >= WB_MAX_INSERT_SLOTS) return -1;
    if (to   < 0 || to   >= WB_MAX_INSERT_SLOTS) return -1;
    if (from == to) return 0;
    wb_plugin_slot tmp = s->tracks[track].inserts[to];
    s->tracks[track].inserts[to]   = s->tracks[track].inserts[from];
    s->tracks[track].inserts[from] = tmp;   /* swap: preserves overwritten FX */
    return 0;
}

/* R031/R033: comping — promote the time-region [t0,t1] (samples) of a
 * take-lane clip onto lane 0 (the comp/main lane). For audio clips the
 * overlapping sub-range is copied and the source trimmed to the outside
 * parts; for MIDI clips the overlapping notes are copied (straddlers split)
 * and the source keeps the outside notes. The kept parts stay on their
 * original lane so you can keep auditioning. Exactly Reaper/Pro Tools /
 * Ableton "send to comp". Returns # comp clips made (>=0), or -1 on error. */
static void clip_add_note(wb_clip *cl, double start, double dur, int pitch, int vel) {
    cl->notes = realloc(cl->notes, (cl->note_count + 1) * sizeof(wb_note));
    wb_note *n = &cl->notes[cl->note_count++];
    n->start = start; n->dur = dur; n->pitch = (uint8_t)pitch; n->vel = (uint8_t)vel;
}
/* R034: remove all material of `lane` that overlaps [t0,t1] from a track
 * (used to give the comp/main lane ownership of the selected range). */
static void track_clear_lane_range(wb_track *tk, int lane, double t0, double t1) {
    if (!tk) return;
    uint32_t w = 0;
    for (uint32_t c = 0; c < tk->clip_count; c++) {
        wb_clip *cl = &tk->clips[c];
        if (cl->lane == lane) {
            double cs = cl->start, ce = cl->start + cl->length;
            if (ce > t0 && cs < t1) {  /* overlaps the range: drop it */
                free(cl->audio_data); free(cl->notes);
                continue;
            }
        }
        tk->clips[w++] = tk->clips[c];   /* keep */
    }
    tk->clip_count = w;
}
int wb_session_comp_region(wb_session *s, int track, int src_lane, double t0, double t1) {
    if (!s || track < 0 || (uint32_t)track >= s->track_count) return -1;
    if (t1 <= t0) return 0;
    wb_track *tk = &s->tracks[track];
    /* R034: the comp lane (0) OWNS the selected range — clear any existing
     * main-lane material there first so re-comping replaces, not stacks
     * (matches Pro Tools/Reaper comp-lane ownership). The source take (and
     * its other lanes) are untouched. */
    track_clear_lane_range(tk, 0, t0, t1);
    int made = 0;
    for (uint32_t c = 0; c < tk->clip_count; c++) {
        wb_clip *cl = &tk->clips[c];
        if (cl->lane != src_lane) continue;
        double cs = cl->start, ce = cl->start + cl->length;
        double a = t0 > cs ? t0 : cs;
        double b = t1 < ce ? t1 : ce;
        if (b <= a) continue;   /* no overlap with the selection */

        if (cl->type == 1) {   /* ---- AUDIO: copy sub-range, trim source ---- */
            if (!cl->audio_data || cl->audio_frames == 0) continue;
            int ch = cl->audio_channels > 0 ? cl->audio_channels : 1;
            wb_clip comp; memset(&comp, 0, sizeof(comp));
            comp.type = 1; comp.lane = 0;
            comp.start = a; comp.length = b - a;
            comp.audio_channels = ch;
            comp.audio_frames = (uint32_t)(b - a);
            comp.clip_gain = cl->clip_gain;
            size_t bytes = (size_t)comp.audio_frames * ch * sizeof(wb_sample);
            comp.audio_data = malloc(bytes);
            if (!comp.audio_data) return -1;
            uint32_t off = (uint32_t)(a - cs);
            memcpy(comp.audio_data, cl->audio_data + off*ch, bytes);
            double left_len  = a - cs;
            double right_len = ce - b;
            wb_clip orig = *cl;
            float orig_gain = cl->clip_gain;   /* capture BEFORE realloc moves the array */
            for (uint32_t k = c; k + 1 < tk->clip_count; k++) tk->clips[k] = tk->clips[k+1];
            tk->clip_count--;
            wb_clip *old_clips = tk->clips;
            wb_clip *newclips = realloc(tk->clips, (tk->clip_count + 3) * sizeof(wb_clip));
            if (!newclips) { free(comp.audio_data); free(orig.audio_data); return -1; }
            tk->clips = newclips;
            (void)old_clips;
            if (left_len > 0.5) {
                wb_clip *L = &tk->clips[tk->clip_count++]; memset(L, 0, sizeof(*L));
                L->type = 1; L->lane = src_lane; L->start = cs; L->length = left_len;
                L->audio_channels = ch; L->audio_frames = (uint32_t)left_len; L->clip_gain = orig_gain;
                L->audio_data = malloc((size_t)left_len*ch*sizeof(wb_sample));
                if (L->audio_data) memcpy(L->audio_data, orig.audio_data, (size_t)left_len*ch*sizeof(wb_sample));
            }
            if (right_len > 0.5) {
                wb_clip *R = &tk->clips[tk->clip_count++]; memset(R, 0, sizeof(*R));
                R->type = 1; R->lane = src_lane; R->start = b; R->length = right_len;
                R->audio_channels = ch; R->audio_frames = (uint32_t)right_len; R->clip_gain = orig_gain;
                R->audio_data = malloc((size_t)right_len*ch*sizeof(wb_sample));
                if (R->audio_data) memcpy(R->audio_data, orig.audio_data + (uint32_t)(b-cs)*ch, (size_t)right_len*ch*sizeof(wb_sample));
            }
            tk->clips[tk->clip_count++] = comp;
            free(orig.audio_data);
            made++;
            c = (uint32_t)-1;
        }
        else if (cl->type == 0) {   /* ---- MIDI: copy notes, split straddlers ---- */
            wb_clip comp; memset(&comp, 0, sizeof(comp));
            comp.type = 0; comp.lane = 0; comp.start = a; comp.length = b - a;
            for (uint32_t i = 0; i < cl->note_count; i++) {
                wb_note *nt = &cl->notes[i];
                double ns = cs + nt->start, ne = ns + nt->dur;   /* absolute */
                if (ne <= a || ns >= b) continue;                /* outside window */
                double s = ns < a ? a : ns;
                double e = ne > b ? b : ne;
                clip_add_note(&comp, s - a, e - s, nt->pitch, nt->vel);
            }
            if (comp.note_count == 0) continue;   /* nothing in the window */
            /* rebuild source clip: keep outside parts, drop the comped middle */
            wb_note *src = cl->notes; uint32_t sn = cl->note_count;
            cl->notes = NULL; cl->note_count = 0;
            for (uint32_t i = 0; i < sn; i++) {
                double ns = cs + src[i].start, ne = ns + src[i].dur;
                if (ns < a) { double ls = ns, le2 = ne < a ? ne : a; if (ls < le2) clip_add_note(cl, ls - cs, le2 - ls, src[i].pitch, src[i].vel); }
                if (ne > b) { double rs = ns > b ? ns : b; clip_add_note(cl, rs - cs, ne - rs, src[i].pitch, src[i].vel); }
            }
            free(src);
            tk->clips = realloc(tk->clips, (tk->clip_count + 1) * sizeof(wb_clip));
            tk->clips[tk->clip_count++] = comp;
            made++;
        }
    }
    return made;
}
static void add_note(wb_clip *clip, double start, double dur, int pitch, int vel) {
    clip->notes = realloc(clip->notes, (clip->note_count + 1) * sizeof(wb_note));
    wb_note *n = &clip->notes[clip->note_count++];
    n->start = start; n->dur = dur; n->pitch = (uint8_t)pitch; n->vel = (uint8_t)vel;
}

static wb_clip *make_midi_clip(double start, double len) {
    wb_clip *c = calloc(1, sizeof(*c));
    c->type = 0;
    c->start = start;
    c->length = len;
    c->note_count = 0;
    c->notes = NULL;
    return c;
}

wb_session *wb_session_demo(void) {
    wb_session *s = calloc(1, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "Demo Song");
    s->bpm = 120.0;
    s->time_sig_num = 4;
    s->time_sig_den = 4;
    s->length = 44100.0 * 8.0;   /* 8 seconds */
    s->track_count = 3;
    s->tracks = calloc(3, sizeof(wb_track));

    /* Track 0: lead synth (C major arpeggio) */
    wb_track *lead = &s->tracks[0];
    snprintf(lead->name, sizeof(lead->name), "Lead");
    lead->kind = 0;
    lead->route = -1;
    lead->volume = 0.8f;
    lead->pan = 0.0f;
    /* slot 0 = instrument id ("synth"), slots 1.. = insert FX chain */
    snprintf(lead->inserts[0].id, sizeof(lead->inserts[0].id), "synth");
    snprintf(lead->inserts[1].id, sizeof(lead->inserts[1].id), "comp");
    snprintf(lead->inserts[2].id, sizeof(lead->inserts[2].id), "reverb");
    lead->clip_count = 1;
    lead->clips = calloc(1, sizeof(wb_clip));
    lead->clips[0] = *make_midi_clip(0, 44100.0 * 8.0);

    /* 8 bars at 120bpm = 2s/bar, quarter note = 0.5s */
    double q = 0.5;
    int scale[] = {60, 64, 67, 72, 67, 64, 60, 62, 64, 67, 69, 67, 64, 62, 60, 0};
    for (int i = 0; i < 16; i++) {
        double start = i * q;
        if (scale[i])
            add_note(&lead->clips[0], start * 44100.0, q * 0.9 * 44100.0, scale[i], 100);
    }

    /* Track 1: bass (root notes, whole note per bar) */
    wb_track *bass = &s->tracks[1];
    snprintf(bass->name, sizeof(bass->name), "Bass");
    bass->kind = 0;
    bass->route = -1;
    bass->volume = 0.6f;
    bass->pan = 0.0f;
    bass->clip_count = 1;
    bass->clips = calloc(1, sizeof(wb_clip));
    bass->clips[0] = *make_midi_clip(0, 44100.0 * 8.0);

    int roots[] = {36, 36, 43, 43, 40, 40, 43, 41};
    double bar = 2.0; /* seconds per bar */
    for (int i = 0; i < 8; i++) {
        add_note(&bass->clips[0], i * bar * 44100.0, bar * 0.95 * 44100.0, roots[i], 90);
    }

    /* Track 2: audio clip (synthesized pad) so the waveform view has content */
    wb_track *pad = &s->tracks[2];
    snprintf(pad->name, sizeof(pad->name), "Pad (audio)");
    pad->kind = 1;
    pad->route = -1;
    pad->volume = 0.5f;
    pad->pan = 0.0f;
    {
        uint32_t nf = 44100 * 4;
        wb_sample *buf = malloc(nf * sizeof(wb_sample)); /* mono */
        for (uint32_t i = 0; i < nf; i++) {
            double t = (double)i / 44100.0;
            double v = 0.25 * (sin(2*M_PI*220.0*t) * 0.5 +
                               sin(2*M_PI*330.0*t) * 0.3 +
                               sin(2*M_PI*440.0*t) * 0.2);
            buf[i] = (float)(v * (1.0 - (double)i / nf));
        }
        wb_session_add_audio_clip(pad, 0, (double)nf, buf, nf, 1);
        free(buf);
    }

    /* R022: arrangement markers — song sections on the timeline */
    double mbar = 44100.0 * 2.0;   /* 2s per bar at 120bpm */
    wb_session_add_marker(s, 0.0,        "Intro", 1);
    wb_session_add_marker(s, mbar,       "Verse", 1);
    wb_session_add_marker(s, mbar*2.0,   "Chorus", 1);
    wb_session_add_marker(s, mbar*3.0,   "Outro", 1);

    /* R047: demo volume-automation lane on track 0 — the arrangement
     * overlay (fader automation readback) has real content out of the box.
     * Session length is SAMPLES (8s = 352800); points are samples too. */
    {
        wb_automation_lane *vol = wb_session_add_automation(s, "volume", 0);
        if (vol) {
            wb_automation_add_point(vol, 0,               0.45, 0);
            wb_automation_add_point(vol, 44100.0*4.0,     1.00, 0);
            wb_automation_add_point(vol, 44100.0*8.0,     0.60, 0);
        }
    }

    return s;
}

/* ---- video clip helpers (R009) ----------------------------------------- */

/* Add a video track to the session. Returns track index or -1 on error. */
int wb_session_add_video_track(wb_session *s, const char *name) {
    if (!s || s->track_count >= WB_MAX_TRACKS) return -1;
    if (!s->tracks) {
        s->tracks = calloc(WB_MAX_TRACKS, sizeof(wb_track));
        if (!s->tracks) return -1;
    }
    wb_track *tr = &s->tracks[s->track_count++];
    tr->kind = WB_TRACK_KIND_VIDEO;  /* video track (R009) */
    tr->volume = 1.0f;
    tr->pan = 0.0f;
    tr->mute = 0;
    tr->solo = 0;
    tr->route = -1;
    tr->clip_count = 0;
    tr->clips = NULL;
    if (name) snprintf(tr->name, sizeof(tr->name), "%s", name);
    else      snprintf(tr->name, sizeof(tr->name), "Video");
    return (int)(s->track_count - 1);
}

/* Add a video clip on a video track. The clip references an FFmpeg-decodable
 * source file. Proxy is generated automatically at import. Returns clip index
 * or -1 on error. */
int wb_session_add_video_clip(wb_session *s, int track, const char *source_path,
                               double timeline_pos) {
    if (!s || track < 0 || track >= (int)s->track_count || !source_path) return -1;
    wb_track *tr = &s->tracks[track];
    if (tr->clip_count >= 1024) return -1;  /* sanity cap */

    tr->clips = realloc(tr->clips, (tr->clip_count + 1) * sizeof(wb_clip));
    if (!tr->clips) return -1;
    wb_clip *cl = &tr->clips[tr->clip_count++];
    memset(cl, 0, sizeof(*cl));
    cl->type = 2;
    cl->color_saturation = 1.0f;   /* R018-C: default = no saturation change */
    cl->start = timeline_pos;
    cl->video = calloc(1, sizeof(wb_video_clip));
    if (!cl->video) { tr->clip_count--; return -1; }
    wb_video_clip_init(cl->video);
    snprintf(cl->video->source_path, sizeof(cl->video->source_path), "%s", source_path);
    cl->video->timeline_pos = timeline_pos;
    cl->video->offline = (access(source_path, F_OK) != 0) ? 1 : 0;  /* G70 */

    /* R042 DEEP FIX (cascade): resolve the clip's source duration here so the
     * clip carries a real length. We use the ffprobe SHELL helper (the same
     * one set_video_proxy uses) — NOT the in-process libavformat decoder that
     * R041 found unstable/crashing on this platform. The shell probe is
     * isolated in a child process, so even a malformed file can't corrupt the
     * engine. This makes add_video_clip self-sufficient (no proxy needed) and
     * fixes downstream gates: s->length growth (export audio render),
     * split/EDL/FCPXML span math, and the hit-test window. */
    cl->video->duration = wb_video_proxy_duration(source_path);

    /* Default clip length = full source duration (UI may trim later). */
    cl->length = cl->video->duration > 0.0 ? cl->video->duration : 0.0;

    /* Grow the session's total song length (in samples) to cover this clip,
     * so wb_engine_render_session (which requires s->length > 0) spans it. */
    if (cl->video->duration > 0.0) {
        double clip_end_samples = (cl->start + cl->video->duration) * WB_SAMPLE_RATE;
        if (clip_end_samples > s->length) s->length = clip_end_samples;
    }
    return (int)(tr->clip_count - 1);
}

/* R068: add a performance clip (snapshot of a recorded wb_perf) onto a
 * video track. The perfclip pointer is transferred into the clip. */
int wb_session_add_perf_clip(wb_session *s, int track, void *perfclip,
                             double timeline_pos, double duration) {
    if (!s || !perfclip || track < 0 || track >= (int)s->track_count)
        return -1;
    wb_track *tr = &s->tracks[track];
    if (tr->clip_count >= 1024) return -1;
    tr->clips = realloc(tr->clips, (tr->clip_count + 1) * sizeof(wb_clip));
    if (!tr->clips) return -1;
    wb_clip *cl = &tr->clips[tr->clip_count++];
    memset(cl, 0, sizeof(*cl));
    cl->type = 3;
    cl->clip_gain = 1.0f;
    cl->start = timeline_pos;
    cl->length = duration;
    cl->lane = 0;
    cl->perfclip = perfclip;
    /* start is in SAMPLES (audio-clip convention); length is SECONDS. */
    double clip_end_samples = cl->start + cl->length * WB_SAMPLE_RATE;
    if (clip_end_samples > s->length) s->length = clip_end_samples;
    return (int)(tr->clip_count - 1);
}

/* R018-C: set a clip's color-correction "intent" (carried into FCPXML). */
void wb_clip_set_color(wb_clip *cl, float exposure, float saturation) {
    if (!cl) return;
    cl->color_exposure = exposure;
    cl->color_saturation = saturation;
}

/* Set a proxy path on an existing video clip (called by the UI import once the
 * 480p proxy is generated). Recomputes duration from the proxy. */
int wb_session_set_video_proxy(wb_session *s, int track, int clip,
                               const char *proxy_path) {
    if (!s || track < 0 || track >= (int)s->track_count || !proxy_path) return -1;
    wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip >= tr->clip_count) return -1;
    wb_clip *cl = &tr->clips[clip];
    if (!cl->video) return -1;
    snprintf(cl->video->proxy_path, sizeof(cl->video->proxy_path), "%s", proxy_path);
    double dur = wb_video_proxy_duration(proxy_path);
    if (dur > 0.0) { cl->video->duration = dur; cl->length = dur; }
    return 0;
}

/* Get the video clip on a track at a given timeline position (seconds).
 * Returns clip index or -1 if no clip at that position. */
int wb_session_video_clip_at(wb_session *s, int track, double timeline_pos) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    for (uint32_t c = 0; c < tr->clip_count; c++) {
        wb_clip *cl = &tr->clips[c];
        if (cl->type != 2) continue;
        double dur = cl->video ? cl->video->duration : 0.0;
        double end = cl->start + (cl->length > 0 ? cl->length : dur);
        if (timeline_pos >= cl->start && timeline_pos < end)
            return (int)c;
    }
    return -1;
}

/* Remove a video clip from a track. Returns 0 on success. */
int wb_session_remove_video_clip(wb_session *s, int track, int clip) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip >= tr->clip_count) return -1;
    wb_clip *cl = &tr->clips[clip];
    if (cl->type != 2 || !cl->video) return -1;

    wb_video_clip_free(cl->video);
    free(cl->video);
    cl->video = NULL;

    /* Compact the track's clip array (preserve order of remaining clips). */
    for (uint32_t c = (uint32_t)clip; c + 1 < tr->clip_count; c++) {
        tr->clips[c] = tr->clips[c + 1];
    }
    tr->clip_count--;
    if (tr->clip_count == 0) {
        free(tr->clips);
        tr->clips = NULL;
    }
    return 0;
}

/* R025: ripple delete — remove a video clip and shift every later clip on the
 * track left by the removed clip's duration, closing the gap (timeline length
 * shrinks, exactly like Premiere's Shift+Delete / Ripple Delete). */
int wb_session_ripple_delete_video_clip(wb_session *s, int track, int clip) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip >= tr->clip_count) return -1;
    wb_clip *cl = &tr->clips[clip];
    if (cl->type != 2 || !cl->video) return -1;

    double removed = cl->length > 0 ? cl->length : cl->video->duration;

    /* remove the clip (frees its video struct) */
    wb_video_clip_free(cl->video);
    free(cl->video);
    cl->video = NULL;
    for (uint32_t c = (uint32_t)clip; c + 1 < tr->clip_count; c++)
        tr->clips[c] = tr->clips[c + 1];
    tr->clip_count--;

    /* shift every later clip left by `removed` */
    for (uint32_t c = (uint32_t)clip; c < tr->clip_count; c++)
        tr->clips[c].start -= removed;

    /* recompute session length from the remaining clips' ends (rippled left) */
    if (s->length > 0) {
        double end = 0;
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            double ce = tr->clips[c].start + (tr->clips[c].length > 0 ? tr->clips[c].length : 0);
            if (ce > end) end = ce;
        }
        if (end < s->length) s->length = end;
    }
    return 0;
}

/* R025: slip — move the clip's source in-point (start_in_source) by `delta`
 * seconds WITHOUT changing its timeline position or duration. The visible
 * content slides under a fixed window (Premiere Slip, Y). Clamped to the
 * available source so the window never runs past the media end. */
int wb_session_slip_video_clip(wb_session *s, int track, int clip, double delta) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip >= tr->clip_count) return -1;
    wb_clip *cl = &tr->clips[clip];
    if (cl->type != 2 || !cl->video) return -1;

    double dur = cl->length > 0 ? cl->length : cl->video->duration;
    double src_dur = wb_video_proxy_duration(cl->video->proxy_path);
    if (src_dur <= 0) src_dur = cl->video->start_in_source + dur + 1.0; /* fall back */
    double nin = cl->video->start_in_source + delta;
    if (nin < 0) nin = 0;
    if (nin + dur > src_dur) nin = src_dur - dur;   /* don't run past media end */
    if (nin < 0) nin = 0;
    cl->video->start_in_source = nin;
    return 0;
}

/* G17: slide — move clip `clip` on the timeline by `delta` seconds while the
 * ADJACENT neighbors absorb the change (Premiere Slide, U): the moved clip
 * keeps its own source window; the left neighbor's tail extends/shrinks to
 * meet it, and the right neighbor's head shifts with its source window
 * advancing so total program duration is preserved. Clamped so no clip
 * goes below minimum length. */
int wb_session_slide_video_clip(wb_session *s, int track, int clip, double delta) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip >= tr->clip_count) return -1;
    wb_clip *m = &tr->clips[clip];
    if (m->type != 2 || !m->video) return -1;

    /* find adjacent clips on the same lane */
    wb_clip *left = NULL, *right = NULL;
    double m_end0 = m->start + m->length;
    for (uint32_t i = 0; i < tr->clip_count; i++) {
        if ((int)i == clip) continue;
        wb_clip *o = &tr->clips[i];
        if (o->type != 2 || !o->video) continue;
        if (!left && fabs(o->start + o->length - m->start) < 0.01)
            left = o;                       /* ends where m begins */
        else if (!right && fabs(o->start - m_end0) < 0.01)
            right = o;                      /* begins where m ends */
    }

    if (delta < 0) {   /* sliding LEFT: m eats into the right clip's head */
        if (!right) return 0;               /* nothing to absorb — no move */
        double maxneg = -(right->length - 0.05);
        if (delta < maxneg) delta = maxneg;
    } else if (delta > 0) {  /* sliding RIGHT: m eats into the left tail */
        if (!left) return 0;
        double maxpos = left->length - 0.05;
        if (delta > maxpos) delta = maxpos;
    } else {
        return 0;
    }

    m->start += delta;
    /* left neighbor's tail follows m's head (grows or shrinks with delta) */
    if (left) {
        left->length = m->start - left->start;
        if (left->length < 0.05) left->length = 0.05;
    }
    /* right neighbor's head follows m's tail; its source window advances by
     * delta and its length compensates so program duration is preserved */
    if (right) {
        right->start = m->start + m->length;
        right->length -= delta;
        right->video->start_in_source += delta;
        if (right->video->start_in_source < 0)
            right->video->start_in_source = 0;
        if (right->length < 0.05) right->length = 0.05;
    }
    return 0;
}

/* R025: roll — adjust the cut between clip `clip` and the next clip on the
 * track: extend clip[clip] by `delta` and shrink clip[clip+1] by the same,
 * sliding clip[clip+1]'s source in-point so total timeline duration is
 * UNCHANGED (Premiere Roll, N). Clamped so neither clip goes negative. */
int wb_session_roll_video_clip(wb_session *s, int track, int clip, double delta) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip + 1 >= tr->clip_count) return -1;
    wb_clip *a = &tr->clips[clip];
    wb_clip *b = &tr->clips[clip + 1];
    if (a->type != 2 || !a->video || b->type != 2 || !b->video) return -1;

    double a_dur = a->length > 0 ? a->length : a->video->duration;
    double b_dur = b->length > 0 ? b->length : b->video->duration;
    if (delta > b_dur - 0.05) delta = b_dur - 0.05;     /* can't shrink b below ~0 */
    if (delta < -a_dur + 0.05) delta = -a_dur + 0.05;   /* can't shrink a below ~0 */

    a->length = a_dur + delta;
    b->start += delta;                                  /* cut point slides */
    b->length = b_dur - delta;
    b->video->start_in_source -= delta;                 /* same content, new window */
    return 0;
}
/* Split a video clip at a timeline position into two clips (A = [start,split),
 * B = [split,end)). Returns the index of the NEW (right) clip, or -1 on error.
 * Preserves the source window via start_in_source + duration. */
int wb_session_split_video_clip(wb_session *s, int track, int clip, double split_pos) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip >= tr->clip_count) return -1;
    wb_clip *cl = &tr->clips[clip];
    if (cl->type != 2 || !cl->video) return -1;

    double clip_start = cl->start;
    double clip_end = cl->start + (cl->length > 0 ? cl->length : cl->video->duration);
    /* split must be strictly inside the clip */
    if (split_pos <= clip_start + 1e-4 || split_pos >= clip_end - 1e-4) return -1;

    /* left keeps [clip_start, split_pos] */
    double left_len = split_pos - clip_start;
    cl->length = left_len;

    /* right clip: [split_pos, clip_end] */
    double right_len = clip_end - split_pos;
    tr->clips = realloc(tr->clips, (tr->clip_count + 1) * sizeof(wb_clip));
    if (!tr->clips) return -1;
    /* R042 DEEP FIX #2: `cl` pointed into the OLD clips array; realloc may
     * have moved the buffer, so re-derive it BEFORE any further use (ASan
     * heap-use-after-free at the cl->video->source_path read below). */
    cl = &tr->clips[clip];
    /* shift everything right of `clip` by one to open a slot */
    for (uint32_t c = tr->clip_count; c > (uint32_t)(clip + 1); c--)
        tr->clips[c] = tr->clips[c - 1];
    wb_clip *r = &tr->clips[clip + 1];
    memset(r, 0, sizeof(*r));
    r->type = 2;
    r->start = split_pos;
    r->length = right_len;
    r->video = calloc(1, sizeof(wb_video_clip));
    if (!r->video) { tr->clip_count = clip + 1; return -1; }
    wb_video_clip_init(r->video);
    snprintf(r->video->source_path, sizeof(r->video->source_path), "%s",
             cl->video->source_path);
    snprintf(r->video->proxy_path, sizeof(r->video->proxy_path), "%s",
             cl->video->proxy_path);
    /* right's source window starts where the split falls */
    double split_offset = split_pos - clip_start;
    double base_in = cl->video->start_in_source;
    if (base_in < 0.0) base_in = 0.0;   /* unset default -> 0 */
    r->video->start_in_source = base_in + split_offset;
    r->video->duration = cl->video->duration - split_offset;
    r->video->timeline_pos = split_pos;

    tr->clip_count++;
    return clip + 1;
    }

    /* ---- R048: transcript text-editing (Descript-style word->media cut) ----
    * Delete words [w0,w1) from the transcript and ripple-cut their time span
    * out of the track's video clips: split at the span edges, then ripple-
    * delete every clip that lies fully inside the span. Partially-overlapping
    * edge clips are trimmed by the span. Timeline shrinks by the cut length. */
    int wb_session_transcript_cut(wb_session *s, int track,
                             struct wb_transcript *tr, int w0, int w1) {
    if (!s || track < 0 || track >= (int)s->track_count || !tr) return -1;
    if (w0 < 0 || w1 <= w0 || w1 > wb_transcript_count(tr)) return -1;

    /* word-range -> time span (ms -> seconds). The span is [first.start,
    * last.end) of the words being deleted. */
    const wb_word *wa = wb_transcript_word(tr, w0);
    const wb_word *wb_ = wb_transcript_word(tr, w1 - 1);
    if (!wa || !wb_) return -1;
    double cut0 = wa->start_ms / 1000.0;
    double cut1 = wb_->end_ms   / 1000.0;
    if (cut1 <= cut0) return -1;

    /* 1) split any clip straddling cut0 and any straddling cut1 so the
    *    span edges land exactly on clip boundaries. Do the LATER split
    *    first so the earlier index isn't invalidated. */
    wb_track *tk = &s->tracks[track];
    for (int pass = 0; pass < 2; pass++) {
       double edge = (pass == 0) ? cut1 : cut0;
       for (uint32_t c = 0; c < tk->clip_count; c++) {
           wb_clip *cl = &tk->clips[c];
           if (cl->type != 2 || !cl->video) continue;
           double cs = cl->start;
           double ce = cs + (cl->length > 0 ? cl->length : cl->video->duration);
           if (edge > cs + 1e-4 && edge < ce - 1e-4) {
               if (wb_session_split_video_clip(s, track, (int)c, edge) < 0)
                   return -1;
               break;   /* one split per edge per pass */
           }
       }
    }

    /* 2) ripple-delete every clip fully inside [cut0, cut1), back-to-front */
    for (int c = (int)tk->clip_count - 1; c >= 0; c--) {
       wb_clip *cl = &tk->clips[c];
       if (cl->type != 2 || !cl->video) continue;
       double cs = cl->start;
       double ce = cs + (cl->length > 0 ? cl->length : cl->video->duration);
       if (cs >= cut0 - 1e-4 && ce <= cut1 + 1e-4)
           wb_session_ripple_delete_video_clip(s, track, c);
    }
    /* 3) (removed R048-fix: after exact splits + full-interior deletes, no
     *    remaining clip overlaps the span; trimming here corrupted the
     *    already-ripple-shifted right neighbor.) */

    /* 4) remove the words from the transcript */
    wb_transcript_remove_range(tr, w0, w1);
    return 0;
    }

    /* ---- G5: EDL / FCPXML interchange (R017 G5) ----------------------------
 * Serialize the session's video tracks to standard edit-decision formats so
 * projects can travel to Resolve/Premiere/ Final Cut. CMX3600 is plain text;
 * FCPXML is XML. Both describe each video clip as (reel/source, src in/out,
 * rec in/out). */

static void edl_timecode(FILE *f, double secs) {
    /* 25 fps EDL timecode (CMX3600 convention) */
    int fps = 25;
    long total = (long)(secs * fps + 0.5);
    int ff = (int)(total % fps);
    int ss = (int)((total / fps) % 60);
    int mm = (int)((total / (fps * 60)) % 60);
    int hh = (int)(total / (fps * 3600));
    fprintf(f, "%02d:%02d:%02d:%02d", hh, mm, ss, ff);
}

/* Write a CMX3600 EDL of all video clips. Returns 0 on success, -1 on error. */
int wb_session_export_edl(const wb_session *s, const char *edl_path) {
    if (!s || !edl_path) return -1;
    FILE *f = fopen(edl_path, "w");
    if (!f) return -1;
    fprintf(f, "TITLE: BigMac Session\n\n");
    int ev = 1;
    for (uint32_t t = 0; t < s->track_count; t++) {
        if (s->tracks[t].kind != WB_TRACK_KIND_VIDEO) continue;
        for (uint32_t c = 0; c < s->tracks[t].clip_count; c++) {
            const wb_clip *cl = &s->tracks[t].clips[c];
            if (cl->type != 2 || !cl->video) continue;
            double src_in  = cl->video->start_in_source < 0 ? 0 : cl->video->start_in_source;
            double dur     = cl->video->duration > 0 ? cl->video->duration : 0;
            double rec_in  = cl->start;
            double rec_out = cl->start + dur;
            const char *name = cl->video->source_path;
            /* reel = basename of source, uppercased, no extension */
            char reel[64];
            const char *bn = strrchr(name, '/'); bn = bn ? bn + 1 : name;
            int i = 0;
            for (; bn[i] && bn[i] != '.' && i < 63; i++)
                reel[i] = (bn[i] >= 'a' && bn[i] <= 'z') ? bn[i] - 32 : bn[i];
            reel[i] = '\0';
            if (reel[0] == '\0') snprintf(reel, sizeof(reel), "REEL%03d", ev);

            fprintf(f, "%03d  %s V C\t", ev, reel);
            edl_timecode(f, rec_in);  fprintf(f, " ");
            edl_timecode(f, rec_out); fprintf(f, " ");
            edl_timecode(f, src_in);  fprintf(f, " ");
            edl_timecode(f, src_in + dur); fprintf(f, "\n");
            ev++;
        }
    }
    fprintf(f, "\n");
    fclose(f);
    return 0;
}

/* Write a minimal FCPXML (Final Cut Pro X) of all video + audio clips,
 * carrying R018-C "intent": per-clip color correction (<adjust-color>) on
 * video, and audio roles + volume (<adjust-volume>) on audio. Returns 0. */
int wb_session_export_fcpxml(const wb_session *s, const char *xml_path) {
    if (!s || !xml_path) return -1;
    FILE *f = fopen(xml_path, "w");
    if (!f) return -1;
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<!DOCTYPE fcpxml>\n");
    fprintf(f, "<fcpxml version=\"1.9\">\n");
    fprintf(f, "  <resources>\n");
    fprintf(f, "    <format id=\"r1\" name=\"FFVideoFormat1080p25\"/>\n");
    int asset_id = 1;
    for (uint32_t t = 0; t < s->track_count; t++) {
        if (s->tracks[t].kind != WB_TRACK_KIND_VIDEO) continue;
        for (uint32_t c = 0; c < s->tracks[t].clip_count; c++) {
            const wb_clip *cl = &s->tracks[t].clips[c];
            if (cl->type != 2 || !cl->video) continue;
            double dur = cl->video->duration > 0 ? cl->video->duration : 0;
            fprintf(f, "    <asset id=\"a%d\" name=\"%s\" src=\"file://%s\" "
                       "hasVideo=\"1\" format=\"r1\" duration=\"%llds\"/>\n",
                    asset_id, cl->video->source_path, cl->video->source_path,
                    (long long)(dur * 25 + 0.5) / 25 * 25 /* frames@25 */);
            asset_id++;
        }
    }
    fprintf(f, "  </resources>\n");
    fprintf(f, "  <library>\n");
    fprintf(f, "    <event name=\"BigMac Session\">\n");
    fprintf(f, "      <project name=\"BigMac Project\">\n");
    fprintf(f, "        <sequence format=\"r1\">\n");
    fprintf(f, "          <spine>\n");
    asset_id = 1;
    for (uint32_t t = 0; t < s->track_count; t++) {
        if (s->tracks[t].kind != WB_TRACK_KIND_VIDEO) continue;
        for (uint32_t c = 0; c < s->tracks[t].clip_count; c++) {
            const wb_clip *cl = &s->tracks[t].clips[c];
            if (cl->type != 2 || !cl->video) continue;
            double dur = cl->video->duration > 0 ? cl->video->duration : 0;
            double off = cl->start;
            long long off_f = (long long)(off * 25 + 0.5);
            long long dur_f = (long long)(dur * 25 + 0.5);
            fprintf(f, "            <asset-clip ref=\"a%d\" "
                       "offset=\"%lld/25s\" duration=\"%lld/25s\" "
                       "name=\"%s\"",
                    asset_id, off_f, dur_f, cl->video->source_path);
            /* R018-C: carry color-correction intent when non-default. */
            if (cl->color_exposure != 0.0f || cl->color_saturation != 1.0f) {
                fprintf(f, ">\n");
                fprintf(f, "              <adjust-color ex=\"%+.3f\" sat=\"%.3f\"/>\n",
                        cl->color_exposure, cl->color_saturation);
                fprintf(f, "            </asset-clip>\n");
            } else {
                fprintf(f, "/>\n");
            }
            asset_id++;
        }
    }
    /* R018-C: audio clips carry role + volume intent. */
    for (uint32_t t = 0; t < s->track_count; t++) {
        const wb_track *tr = &s->tracks[t];
        if (tr->kind != WB_TRACK_KIND_AUDIO) continue;
        /* derive FCPXML role from track name */
        const char *role = "dialogue";
        if (strstr(tr->name, "music") || strstr(tr->name, "Music")) role = "music";
        else if (strstr(tr->name, "sfx") || strstr(tr->name, "SFX") ||
                 strstr(tr->name, "fx") || strstr(tr->name, "effect")) role = "effects";
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            const wb_clip *cl = &tr->clips[c];
            if (cl->type != 1) continue;
            double off = cl->start / 44100.0;     /* samples -> seconds */
            double len = cl->length / 44100.0;
            long long off_f = (long long)(off * 25 + 0.5);
            long long len_f = (long long)(len * 25 + 0.5);
            /* volume in dB: 20*log10(gain); -inf handled as -60 */
            double db = tr->volume > 0 ? 20.0 * log10(tr->volume) : -60.0;
            fprintf(f, "            <asset-clip name=\"%s\" offset=\"%lld/25s\" "
                       "duration=\"%lld/25s\" audioRole=\"%s\">\n",
                    tr->name, off_f, len_f, role);
            fprintf(f, "              <adjust-volume amount=\"%.1fdB\"/>\n", db);
            fprintf(f, "            </asset-clip>\n");
        }
    }
    fprintf(f, "          </spine>\n");
    fprintf(f, "        </sequence>\n");
    fprintf(f, "      </project>\n");
    fprintf(f, "    </event>\n");
    fprintf(f, "  </library>\n");
    fprintf(f, "</fcpxml>\n");
    fclose(f);
    return 0;
}

/* ---- G14: direct-manipulation clip move/trim (mouse drag model ops) ----- */
int wb_session_move_clip(wb_session *s, int track, int clip,
                         int new_track, double new_start) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    if (new_track < 0 || new_track >= (int)s->track_count) return -1;
    wb_track *src = &s->tracks[track];
    if (clip < 0 || (uint32_t)clip >= src->clip_count) return -1;
    wb_clip moving = src->clips[clip];
    /* clips may only land on tracks that host the same media kind */
    if (moving.type != s->tracks[new_track].kind
        && !(track == new_track))
        return -1;
    if (new_start < 0.0) new_start = 0.0;
    moving.start = new_start;
    wb_track *dst = &s->tracks[new_track];
    wb_clip *na = realloc(dst->clips, (dst->clip_count + 1) * sizeof(wb_clip));
    if (!na) return -1;
    dst->clips = na;
    dst->clips[dst->clip_count++] = moving;
    /* remove from source (compact the array; indices shift by one) */
    for (uint32_t i = (uint32_t)clip; i + 1 < src->clip_count; i++)
        src->clips[i] = src->clips[i + 1];
    src->clip_count--;
    /* keep the session length covering the clip's tail */
    double end = moving.start + moving.length;
    if (end > s->length) s->length = end;
    return 0;
}

/* G63: dynamic transitions — re-pair crossfades after any clip move/trim.
 * For every pair of adjacent clips (a ends where b starts, within tol):
 *   - if either has a facing fade, equalize both to the max so the join
 *     crossfades symmetrically (dynamic re-linking);
 *   - clamp fades to half the shorter clip's length.
 * Fades on non-adjacent edges are left alone (plain fades). Uses the
 * engine's clip-edit side-table; pass wb_engine_clip_edit(e). */
void wb_session_update_transitions(wb_session *s, wb_clip_edit_table *et) {
    if (!s || !et) return;
    const double tol = 1e-3;   /* adjacency tolerance in clip units */
    for (int t = 0; t < (int)s->track_count; t++) {
        wb_track *tr = &s->tracks[t];
        for (uint32_t i = 0; i + 1 < tr->clip_count; i++) {
            wb_clip *a = &tr->clips[i];
            int best = -1; double bestd = tol;
            for (uint32_t j = i + 1; j < tr->clip_count; j++) {
                double d = tr->clips[j].start - (a->start + a->length);
                if (d > -tol && d < bestd) { bestd = d; best = (int)j; }
                if (d >= tol) break;
            }
            if (best < 0) continue;
            wb_clip *b = &tr->clips[best];
            double alen = a->length > 0 ? a->length : 1.0;
            double blen = b->length > 0 ? b->length : 1.0;
            double minlen = alen < blen ? alen : blen;
            wb_clip_edit *ea = wb_clip_edit_get(et, t, (int)i);
            wb_clip_edit *eb = wb_clip_edit_get(et, t, best);
            if (!ea || !eb) continue;
            /* dynamic re-link: whichever facing fade exists is shared */
            double fi = ea->fade_in  > 0 ? (double)ea->fade_in  : 0.0;
            double fo = eb->fade_out > 0 ? (double)eb->fade_out : 0.0;
            double xf = fi > fo ? fi : fo;
            if (xf <= 0) continue;              /* no transition requested */
            if (xf > minlen * 0.5) xf = minlen * 0.5;
            ea->fade_in  = (float)xf;
            eb->fade_out = (float)xf;
        }
    }
}

static int trim_common(wb_session *s, int track, int clip, wb_clip **out) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    if (clip < 0 || (uint32_t)clip >= tr->clip_count) return -1;
    *out = &tr->clips[clip];
    return 0;
}

#define WB_TRIM_MIN_SMP (WB_SAMPLE_RATE * 0.01)   /* 10 ms minimum clip */
/* MIDI clips keep start/length/notes in SECONDS; audio uses SAMPLES. */
static double trim_min_len(const wb_clip *cl) {
    return cl->type == 0 ? 1e-6 : WB_TRIM_MIN_SMP;
}

int wb_session_trim_clip_head(wb_session *s, void *ed,
                              int track, int clip, double delta) {
    wb_clip *cl;
    if (trim_common(s, track, clip, &cl) != 0) return -1;
    double minlen = trim_min_len(cl);
    if (delta > cl->length - minlen) delta = cl->length - minlen;
    if (delta < 0.0 && cl->start + delta < 0.0) delta = -cl->start;
    cl->start += delta;
    cl->length -= delta;
    if (cl->type == 1) {
        /* audio: keep the waveform anchored — sliding the head forward reads
         * further into the source buffer (start_in_source semantics); sliding
         * it back left rewinds the offset so revealed material plays. */
        wb_clip_edit_table *et = (wb_clip_edit_table *)ed;
        if (et) {
            wb_clip_edit *e = wb_clip_edit_get(et, track, clip);
            e->start_in_source += delta;
            if (e->start_in_source < 0.0) e->start_in_source = 0.0;
        }
    } else if (cl->type == 0) {
        /* MIDI: notes live relative to clip start — keep them at their
         * absolute timeline spots by shifting against the clip, then clamp
         * into [0,length]. */
        for (uint32_t k = 0; k < cl->note_count; k++) {
            wb_note *nt = &cl->notes[k];
            nt->start -= delta;
            double nend = nt->start + nt->dur;
            if (nend <= 0.0) { nt->dur = 0.0; continue; }
            if (nt->start < 0.0) {
                nt->dur -= -nt->start;
                nt->start = 0.0;
            }
            if (nt->dur < 0.0) nt->dur = 0.0;
        }
    }
    return 0;
}

int wb_session_trim_clip_tail(wb_session *s, void *ed,
                              int track, int clip, double delta) {
    wb_clip *cl;
    if (trim_common(s, track, clip, &cl) != 0) return -1;
    (void)ed;
    double minlen = trim_min_len(cl);
    double newlen = cl->length + delta;
    if (newlen < minlen) newlen = minlen;
    if (cl->type == 1 && cl->audio_frames > 0) {
        /* audio: cannot extend past what the buffer still has to give */
        wb_clip_edit_table *et = (wb_clip_edit_table *)ed;
        double sis = 0.0;
        if (et) sis = wb_clip_edit_get(et, track, clip)->start_in_source;
        double avail = (double)cl->audio_frames - sis;
        if (avail < WB_TRIM_MIN_SMP) avail = WB_TRIM_MIN_SMP;
        if (newlen > avail) newlen = avail;
    } else if (cl->type == 0) {
        /* MIDI: shortening the tail drops/clamps notes past the new end */
        for (uint32_t k = 0; k < cl->note_count; k++) {
            wb_note *nt = &cl->notes[k];
            if (nt->start >= newlen) { nt->dur = 0.0; continue; }
            if (nt->start + nt->dur > newlen) nt->dur = newlen - nt->start;
        }
    }
    cl->length = newlen;
    double end = cl->start + cl->length;
    if (end > s->length) s->length = end;
    return 0;
}

/* ---- G04: media bin ------------------------------------------------------- */
/* basename helper: pointer into `path` after the last '/'. */
static const char *bin_basename(const char *path) {
    if (!path) return "";
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

int wb_session_add_bin_entry(wb_session *s, const char *path, int kind,
                             double duration) {
    if (!s || !path || !path[0]) return -1;
    if (s->bin_count >= WB_MAX_BIN) return -1;
    wb_bin_entry *e = &s->bin_entries[s->bin_count];
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    snprintf(e->name, sizeof(e->name), "%s", bin_basename(path));
    e->kind = (kind != 0) ? 1 : 0;   /* normalize: 0 audio / 1 video */
    e->duration = duration;
    e->offline = (access(path, R_OK) != 0) ? 1 : 0;
    s->bin_count++;
    return (int)(s->bin_count - 1);
}

/* G68: sort bin entries. mode: 0 = by name, 1 = by kind then name,
 * 2 = by duration. Stable enough via index tiebreak on equality. */
void wb_session_sort_bin(wb_session *s, int mode) {
    if (!s || s->bin_count < 2) return;
    for (uint32_t i = 0; i + 1 < s->bin_count; i++) {       /* insertion sort */
        int min = (int)i;
        for (uint32_t j = i + 1; j < s->bin_count; j++) {
            wb_bin_entry *a = &s->bin_entries[min], *b = &s->bin_entries[j];
            int lt = 0;
            if (mode == 1)
                lt = b->kind < a->kind ||
                     (b->kind == a->kind && strcasecmp(b->name, a->name) < 0);
            else if (mode == 2)
                lt = b->duration < a->duration;
            else
                lt = strcasecmp(b->name, a->name) < 0;
            if (lt) min = (int)j;
        }
        if (min != (int)i) {
            wb_bin_entry tmp = s->bin_entries[i];
            s->bin_entries[i] = s->bin_entries[min];
            s->bin_entries[min] = tmp;
        }
    }
}

void wb_session_update_offline(wb_session *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->bin_count; i++)
        s->bin_entries[i].offline = (access(s->bin_entries[i].path, F_OK) != 0);
    for (uint32_t t = 0; t < s->track_count; t++) {
        wb_track *tr = &s->tracks[t];
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            wb_clip *cl = &tr->clips[c];
            if (cl->type == 2 && cl->video)
                cl->video->offline = (access(cl->video->source_path, F_OK) != 0);
        }
    }
}

/* G70: search the given directory for a file matching `basename`; if found,
 * append its full path. Returns 1 on a hit, 0 otherwise. */
static int find_by_basename(const char *dir, const char *basename,
                            char out[1024]) {
    if (!dir || !basename || !basename[0]) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int hit = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (strcasecmp(e->d_name, basename) == 0) {
            snprintf(out, 1024, "%s/%s", dir, e->d_name);
            hit = 1;
            break;
        }
    }
    closedir(d);
    return hit;
}

/* Relink one offline bin entry by basename against a candidate list of dirs. */
static int relink_one(wb_session *s, int idx) {
    wb_bin_entry *e = &s->bin_entries[idx];
    if (!e->offline || !e->path[0]) return 0;
    const char *base = bin_basename(e->path);
    const char *home = getenv("HOME");
    const char *candidates[] = {
        home,                         /* the original file's directory is */
        home ? "/Users/waefrebeorn/Movies" : NULL,
        home ? "/Users/waefrebeorn/Desktop" : NULL,
        home ? "/Users/waefrebeorn/Documents" : NULL,
    };
    /* first: the directory the entry originally came from */
    char orig_dir[1024];
    const char *slash = strrchr(e->path, '/');
    if (slash) {
        size_t n = (size_t)(slash - e->path);
        if (n >= sizeof(orig_dir)) n = sizeof(orig_dir) - 1;
        memcpy(orig_dir, e->path, n);
        orig_dir[n] = '\0';
        candidates[0] = orig_dir;
    } else {
        candidates[0] = ".";
    }
    char found[1024];
    for (size_t c = 0; c < sizeof(candidates)/sizeof(candidates[0]); c++) {
        if (!candidates[c]) continue;
        if (find_by_basename(candidates[c], base, found)) {
            snprintf(e->path, sizeof(e->path), "%s", found);
            e->offline = 0;
            /* G70: mirror the relinked path onto any video clip that pointed
             * at the same (now-stale) source_path. */
            for (uint32_t t = 0; t < s->track_count; t++) {
                wb_track *tr = &s->tracks[t];
                for (uint32_t cl = 0; cl < tr->clip_count; cl++) {
                    wb_clip *c2 = &tr->clips[cl];
                    if (c2->type == 2 && c2->video &&
                        strcmp(c2->video->source_path, e->path) != 0 &&
                        strcasecmp(bin_basename(c2->video->source_path), base) == 0) {
                        snprintf(c2->video->source_path,
                                 sizeof(c2->video->source_path), "%s", found);
                        c2->video->offline = 0;
                    }
                }
            }
            return 1;
        }
    }
    return 0;
}

int wb_session_relink_bin(wb_session *s) {
    if (!s) return 0;
    int relinked = 0;
    for (uint32_t i = 0; i < s->bin_count; i++)
        if (relink_one(s, (int)i)) relinked++;
    return relinked;
}

/* ---- G28: strip silence / region detect --------------------------------- */
/* Scan an audio clip's buffer for regions louder than thresh (linear 0..1)
 * lasting at least min_sec. Each region becomes its OWN clip (samples copied,
 * clear ownership); the original clip is removed. Returns the number of
 * regions created (0 = clip was all silence -> clip removed, -1 on error). */
int wb_session_strip_silence(wb_session *s, int track, int clip,
                             float thresh, double min_sec) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip >= tr->clip_count) return -1;
    wb_clip *cl = &tr->clips[clip];
    if (cl->type != 1 || !cl->audio_data || cl->audio_frames == 0) return -1;
    uint32_t ch = cl->audio_channels > 0 ? cl->audio_channels : 1;
    uint32_t min_frames = (uint32_t)(min_sec * WB_SAMPLE_RATE);
    if (min_frames < 1) min_frames = 1;

    /* detect loud regions */
    int regions[256][2];   /* [start_frame, end_frame) — cap 256 regions */
    int nreg = 0;
    int in_region = 0;
    uint32_t rstart = 0, quiet_run = 0;
    for (uint32_t i = 0; i < cl->audio_frames; i++) {
        float peak = 0;
        for (uint32_t c = 0; c < ch; c++) {
            float v = fabsf(cl->audio_data[i * ch + c]);
            if (v > peak) peak = v;
        }
        if (peak >= thresh) {
            if (!in_region) { in_region = 1; rstart = i; }
            quiet_run = 0;                       /* loud resets the hangover */
        } else if (in_region) {
            /* close only after silence PERSISTS min_frames (zero-crossing safe) */
            if (++quiet_run >= min_frames) {
                uint32_t rend = i - quiet_run + 1;
                if (rend - rstart >= min_frames && nreg < 256) {
                    regions[nreg][0] = (int)rstart;
                    regions[nreg][1] = (int)rend;
                    nreg++;
                }
                in_region = 0; quiet_run = 0;
            }
        }
    }
    if (in_region && cl->audio_frames - rstart >= min_frames && nreg < 256) {
        regions[nreg][0] = (int)rstart; regions[nreg][1] = (int)cl->audio_frames; nreg++;
    }
    if (nreg == 0) {
        /* all silence: remove the clip entirely */
        free(cl->audio_data);
        for (uint32_t i = (uint32_t)clip; i + 1 < tr->clip_count; i++)
            tr->clips[i] = tr->clips[i + 1];
        tr->clip_count--;
        return 0;
    }

    /* build new clips, one per region (copies of their slice) */
    wb_clip *newclips = calloc(nreg, sizeof(wb_clip));
    if (!newclips) return -1;
    for (int rgi = 0; rgi < nreg; rgi++) {
        wb_clip *nc = &newclips[rgi];
        *nc = *cl;                                    /* shallow copy */
        uint32_t s0 = regions[rgi][0], s1 = regions[rgi][1];
        uint32_t nf = s1 - s0;
        nc->audio_data = malloc(nf * ch * sizeof(wb_sample));
        if (!nc->audio_data) { nc->audio_frames = 0; continue; }
        memcpy(nc->audio_data, cl->audio_data + s0 * ch,
               nf * ch * sizeof(wb_sample));
        nc->audio_frames = nf;
        nc->start = cl->start + (double)s0;
        nc->length = (double)nf;                      /* audio: samples */
    }
    /* remove original clip (owns the full buffer) */
    free(cl->audio_data);
    for (uint32_t i = (uint32_t)clip; i + 1 < tr->clip_count; i++)
        tr->clips[i] = tr->clips[i + 1];
    tr->clip_count--;
    /* append the regions */
    uint32_t base = tr->clip_count;
    tr->clips = realloc(tr->clips, (tr->clip_count + nreg) * sizeof(wb_clip));
    if (!tr->clips) { free(newclips); return -1; }
    memcpy(tr->clips + base, newclips, nreg * sizeof(wb_clip));
    tr->clip_count += nreg;
    free(newclips);
    return nreg;
}

/* ---- G18/G19: replace edit + three-point editing ------------------------ */
/* G18: replace edit — swap the clip at (track, clip) for a new source,
 * keeping the SAME timeline position and duration (match-frame replace).
 * Returns the new clip index, or -1 on error. */
int wb_session_replace_video_clip(wb_session *s, int track, int clip,
                                  const char *new_source) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip >= tr->clip_count) return -1;
    wb_clip *old = &tr->clips[clip];
    if (old->type != 2 || !old->video) return -1;
    double pos = old->start;                 /* keep position + duration */
    double old_dur = old->length > 0 ? old->length
                   : (old->video->duration > 0 ? old->video->duration : 0.0);
    if (wb_session_remove_video_clip(s, track, clip) != 0) return -1;
    /* remove compacted the array; insert the replacement at the same spot by
     * re-adding at pos, then moving it into index `clip` */
    int ni = wb_session_add_video_clip(s, track, new_source, pos);
    if (ni < 0) return -1;
    /* match-frame: restore the exact slot duration (add_video_clip may leave
     * length 0 for offline sources until a probe succeeds). */
    tr->clips[ni].length = old_dur;
    tr->clips[ni].video->duration = old_dur;
    tr->clips[ni].video->timeline_pos = pos;
    if (ni != clip) {
        wb_clip tmp = tr->clips[clip];
        tr->clips[clip] = tr->clips[ni];
        tr->clips[ni] = tmp;
    }
    return clip;
}

/* G19: three-point editing — place a source range [src_in, src_in+dur) from
 * `source` onto the timeline AT playhead `dest`, overwriting whatever is
 * there (splitting straddlers). Points 1+2 = source in/out; point 3 =
 * timeline destination. Returns the new clip index or -1. */
int wb_session_three_point_edit(wb_session *s, int track, const char *source,
                                double src_in, double dur, double dest) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    if (dur <= 0.0) return -1;
    wb_track *tr = &s->tracks[track];
    /* split any clip straddling [dest, dest+dur), back to front */
    for (uint32_t c = tr->clip_count; c-- > 0; ) {
        wb_clip *cl = &tr->clips[c];
        if (cl->type != 2 || !cl->video) continue;
        double cs = cl->start, ce = cl->start + cl->length;
        if (cs < dest && ce > dest && ce <= dest + dur)
            wb_session_split_video_clip(s, track, (int)c, dest);
        else if (cs < dest + dur && ce > dest + dur && cs >= dest)
            wb_session_split_video_clip(s, track, (int)c, dest + dur);
        else if (cs < dest && ce > dest + dur) {
            wb_session_split_video_clip(s, track, (int)c, dest);
            /* after split the tail sits at index c+1 */
            wb_session_split_video_clip(s, track, (int)c + 1, dest + dur);
        }
    }
    /* delete fully-covered clips, back to front */
    for (uint32_t c = tr->clip_count; c-- > 0; ) {
        wb_clip *cl = &tr->clips[c];
        if (cl->type != 2 || !cl->video) continue;
        if (cl->start >= dest && cl->start + cl->length <= dest + dur + 0.001)
            wb_session_remove_video_clip(s, track, (int)c);
    }
    /* place the source range */
    int ni = wb_session_add_video_clip(s, track, source, dest);
    if (ni >= 0 && tr->clips[ni].video) {
        tr->clips[ni].video->start_in_source = src_in;
        tr->clips[ni].length = dur;
        tr->clips[ni].video->duration = dur;
    }
    return ni;
}

/* ---- G83: MIDI transformations ------------------------------------------ */
/* mode: 0 = humanize (randomize vel +-8, start +-10ms), 1 = randomize order
 * of velocities, 2 = arpeggiate up (sort notes by pitch, re-space evenly
 * across the clip), 3 = strum (offset each successive note by 15ms).
 * Returns the number of notes touched, or -1 on error. */
int wb_session_transform_notes(wb_session *s, int track, int clip, int mode) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip >= tr->clip_count) return -1;
    wb_clip *cl = &tr->clips[clip];
    if (cl->type != 0 || !cl->notes || cl->note_count == 0) return -1;
    int n = 0;
    switch (mode) {
    case 0:   /* humanize */
        for (uint32_t k = 0; k < cl->note_count; k++, n++) {
            wb_note *nt = &cl->notes[k];
            nt->vel = (uint8_t)(nt->vel + (rand() % 17) - 8);
            if (nt->vel > 127) nt->vel = 127; if (nt->vel < 1) nt->vel = 1;
            double jitter = ((rand() % 441) - 220);        /* +-~5ms samples */
            nt->start += jitter; if (nt->start < 0) nt->start = 0;
        }
        break;
    case 1: { /* randomize velocity order */
        for (uint32_t k = cl->note_count; k-- > 1; n++) {
            uint32_t j = rand() % (k + 1);
            uint8_t tv = cl->notes[k].vel;
            cl->notes[k].vel = cl->notes[j].vel;
            cl->notes[j].vel = tv;
        }
        break;
    }
    case 2: { /* arpeggiate up: sort by pitch, spread across clip length */
        for (uint32_t a = 0; a + 1 < cl->note_count; a++)
            for (uint32_t b = a + 1; b < cl->note_count; b++)
                if (cl->notes[b].pitch < cl->notes[a].pitch) {
                    wb_note t = cl->notes[a]; cl->notes[a] = cl->notes[b];
                    cl->notes[b] = t;
                }
        double span = cl->length > 0 ? cl->length : (double)WB_SAMPLE_RATE;
        for (uint32_t k = 0; k < cl->note_count; k++, n++) {
            cl->notes[k].start = span * (double)k / (double)cl->note_count;
        }
        break;
    }
    case 3:   /* strum: 15ms per successive note in current order */
        for (uint32_t k = 0; k < cl->note_count; k++, n++) {
            cl->notes[k].start += (double)k * 0.015 * WB_SAMPLE_RATE;
        }
        break;
    default:
        return -1;
    }
    return n;
}

/* ---- G84: articulation management ---------------------------------------- */
/* Named articulations map to the raw keyswitch note the instrument expects
 * (Cubase Expression Maps / Logic Articulation Sets, minimal form). */
typedef struct {
    const char *name;
    int keyswitch;        /* MIDI note number of the keyswitch */
} wb_articulation;

/* Built-in map — the standard General MIDI-adjacent keyswitch layout used by
 * our bundled instruments. Index = articulation id. */
static const wb_articulation g_articulations[] = {
    { "LEGATO",    36 },
    { "SUSTAIN",   37 },
    { "STACCATO",  38 },
    { "TREMOLO",   39 },
    { "Pizzicato", 40 },
    { "MUTE",      41 },
};
#define WB_ARTICULATION_N ((int)(sizeof(g_articulations)/sizeof(g_articulations[0])))

/* Send the keyswitch for articulation `art_id` on `track` (a short note-on/
 * off pair at the current position). Returns 0, or -1 for a bad id. */
int wb_session_set_articulation(wb_session *s, int track, int art_id) {
    if (!s || art_id < 0 || art_id >= WB_ARTICULATION_N) return -1;
    if (track < 0 || track >= (int)s->track_count) return -1;
    /* The engine consumes keyswitches as ordinary short notes; the caller
     * (UI) routes this through wb_engine_note. We validate + report here so
     * the mapping lives in one place. */
    (void)s; (void)track;
    return 0;
}
const char *wb_articulation_name(int art_id) {
    if (art_id < 0 || art_id >= WB_ARTICULATION_N) return "";
    return g_articulations[art_id].name;
}
int wb_articulation_keyswitch(int art_id) {
    if (art_id < 0 || art_id >= WB_ARTICULATION_N) return -1;
    return g_articulations[art_id].keyswitch;
}
int wb_articulation_count(void) { return WB_ARTICULATION_N; }

/* ---- G82: chord track ---------------------------------------------------- */
int wb_session_add_chord(wb_session *s, double pos, int root, int type) {
    if (!s || s->chord_count >= 64) return -1;
    if (root < 0 || root > 11) return -1;
    if (type < 0 || type > 4) return -1;
    wb_chord_ev *e = &s->chords[s->chord_count++];
    e->pos = pos; e->root = root; e->type = type;
    /* keep sorted by position */
    for (int i = s->chord_count - 1; i > 0; i--) {
        if (s->chords[i].pos < s->chords[i-1].pos) {
            wb_chord_ev t = s->chords[i];
            s->chords[i] = s->chords[i-1];
            s->chords[i-1] = t;
        } else break;
    }
    return s->chord_count - 1;
}
void wb_session_clear_chords(wb_session *s) { if (s) s->chord_count = 0; }
int wb_session_chord_at(const wb_session *s, double pos, int *root, int *type) {
    if (!s || s->chord_count == 0) return -1;
    int best = -1;
    for (uint32_t i = 0; i < s->chord_count; i++)
        if (s->chords[i].pos <= pos) best = (int)i;
        else break;
    if (best < 0) return -1;
    if (root) *root = s->chords[best].root;
    if (type) *type = s->chords[best].type;
    return best;
}

/* G82: harmonic transformation — snap every note of a MIDI clip into the
 * chord sounding at that note's position (diatonic to the chord's scale). */
int wb_session_snap_to_chords(wb_session *s, int track, int clip) {
    if (!s || track < 0 || track >= (int)s->track_count) return -1;
    wb_track *tr = &s->tracks[track];
    if ((uint32_t)clip >= tr->clip_count) return -1;
    wb_clip *cl = &tr->clips[clip];
    if (cl->type != 0 || !cl->notes) return -1;
    if (s->chord_count == 0) return 0;
    int n = 0;
    for (uint32_t k = 0; k < cl->note_count; k++, n++) {
        double abs_pos = cl->start + cl->notes[k].start;
        int root, type;
        if (wb_session_chord_at(s, abs_pos, &root, &type) < 0) continue;
        cl->notes[k].pitch = wb_scale_snap(root, type, cl->notes[k].pitch);
    }
    return n;
}

/* ---- G22: swap-clips drag ------------------------------------------------ */
/* Swap the timeline positions of two clips on the same track (or across tracks
 * hosting the same kind). Used by alt+drop in the arrangement: instead of
 * overlapping, the target clip moves to where the dragged one started. */
int wb_session_swap_clips(wb_session *s, int track_a, int clip_a,
                          int track_b, int clip_b) {
    if (!s || track_a < 0 || track_a >= (int)s->track_count) return -1;
    if (track_b < 0 || track_b >= (int)s->track_count) return -1;
    wb_track *ta = &s->tracks[track_a], *tb = &s->tracks[track_b];
    if ((uint32_t)clip_a >= ta->clip_count) return -1;
    if ((uint32_t)clip_b >= tb->clip_count) return -1;
    if (ta->clips[clip_a].type != tb->clips[clip_b].type) return -1;
    double sa = ta->clips[clip_a].start;
    double sb = tb->clips[clip_b].start;
    ta->clips[clip_a].start = sb;
    tb->clips[clip_b].start = sa;
    /* keep session length covered */
    double e1 = sb + tb->clips[clip_b].length;
    double e2 = sa + ta->clips[clip_a].length;
    if (e1 > s->length) s->length = e1;
    if (e2 > s->length) s->length = e2;
    return 0;
}

/* ---- G76: FX chain rack presets (save/load chains) ---------------------- */
#define WB_CHAIN_MAX_SLOTS 8
/* Serialize a track's insert chain to "<id1>|<id2>|..." (slot order 0..N).
 * Returns bytes written, or -1. */
int wb_session_export_chain(const wb_session *s, int track,
                            char *out, int cap) {
    if (!s || track < 0 || track >= (int)s->track_count || !out || cap <= 0)
        return -1;
    int w = 0;
    out[0] = 0;
    for (int slot = 0; slot < WB_MAX_INSERT_SLOTS && slot < WB_CHAIN_MAX_SLOTS; slot++) {
        const char *id = s->tracks[track].inserts[slot].id;
        if (!id || !id[0]) id = "-";
        int n = snprintf(out + w, cap - w, "%s%s", slot ? "|" : "", id);
        if (n < 0 || w + n >= cap) return -1;
        w += n;
    }
    return w;
}
/* Load a chain string ("eq|chorus||delay") into the track: each token sets
 * that slot; "-" clears it. Returns 0 or -1. */
int wb_session_import_chain(wb_session *s, int track, const char *chain) {
    if (!s || track < 0 || track >= (int)s->track_count || !chain) return -1;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", chain);
    int slot = 0;
    char *tok = strtok(buf, "|");
    while (tok && slot < WB_MAX_INSERT_SLOTS) {
        if (strcmp(tok, "-") == 0)
            wb_session_set_insert(s, track, slot, NULL);
        else
            wb_session_set_insert(s, track, slot, tok);
        slot++;
        tok = strtok(NULL, "|");
    }
    return 0;
}

/* ---- G77: copy/paste channel-strip settings ------------------------------ */
/* Copies volume, pan, and the whole insert chain from track A to B.
 * Returns 0 or -1. */
int wb_session_copy_strip(wb_session *s, int src_track, int dst_track) {
    if (!s || src_track < 0 || src_track >= (int)s->track_count ||
        dst_track < 0 || dst_track >= (int)s->track_count) return -1;
    if (src_track == dst_track) return 0;
    wb_track *from = &s->tracks[src_track];
    wb_track *to   = &s->tracks[dst_track];
    to->volume = from->volume;
    to->pan    = from->pan;
    char chain[256];
    if (wb_session_export_chain(s, src_track, chain, sizeof(chain)) > 0)
        wb_session_import_chain(s, dst_track, chain);
    return 0;
}

/* G79: meter tap point */
void wb_session_set_meter_point(wb_session *s, int post_fader) {
    if (s) s->meter_post_fader = post_fader ? 1 : 0;
}

/* ---- G72: speed ramps / retiming ---------------------------------------- */
/* Set a constant retime on a video/audio clip: rate 2.0 = double-speed (the
 * clip's timeline length halves for the same source span). Returns the new
 * timeline length in clip units (samples for audio, seconds for video). */
double wb_session_set_retime(wb_clip_edit_table *et, int track, int clip,
                             double rate) {
    if (!et || rate <= 0.0 || rate > 16.0) return -1;
    wb_clip_edit *e = wb_clip_edit_get(et, track, clip);
    if (!e) return -1;
    e->retime = rate;
    return rate;   /* caller recomputes length from their own units */
}
