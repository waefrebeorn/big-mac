/* wb_session.c — session model: build, destroy, and a demo-song builder.
 * A session is the editable arrangement: tracks, clips, notes. The engine
 * consumes it at render time. This file also owns the .wbus project
 * serialization (load/save) — plain text, human-readable.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "wbus.h"
#include "wbus_video.h"

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
    return tr;
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
int wb_session_comp_region(wb_session *s, int track, int src_lane, double t0, double t1) {
    if (!s || track < 0 || (uint32_t)track >= s->track_count) return -1;
    if (t1 <= t0) return 0;
    wb_track *tk = &s->tracks[track];
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
            for (uint32_t k = c; k + 1 < tk->clip_count; k++) tk->clips[k] = tk->clips[k+1];
            tk->clip_count--;
            tk->clips = realloc(tk->clips, (tk->clip_count + 3) * sizeof(wb_clip));
            if (left_len > 0.5) {
                wb_clip *L = &tk->clips[tk->clip_count++]; memset(L, 0, sizeof(*L));
                L->type = 1; L->lane = src_lane; L->start = cs; L->length = left_len;
                L->audio_channels = ch; L->audio_frames = (uint32_t)left_len; L->clip_gain = cl->clip_gain;
                L->audio_data = malloc((size_t)left_len*ch*sizeof(wb_sample));
                if (L->audio_data) memcpy(L->audio_data, orig.audio_data, (size_t)left_len*ch*sizeof(wb_sample));
            }
            if (right_len > 0.5) {
                wb_clip *R = &tk->clips[tk->clip_count++]; memset(R, 0, sizeof(*R));
                R->type = 1; R->lane = src_lane; R->start = b; R->length = right_len;
                R->audio_channels = ch; R->audio_frames = (uint32_t)right_len; R->clip_gain = cl->clip_gain;
                R->audio_data = malloc((size_t)right_len*ch*sizeof(wb_sample));
                if (R->audio_data) memcpy(R->audio_data, orig.audio_data + (uint32_t)(b-cs)*ch, (size_t)right_len*ch*sizeof(wb_sample));
            }
            tk->clips[tk->clip_count++] = comp;
            free(orig.audio_data);
            made++;
            c = (uint32_t)-1;
        }
        else if (cl->type == 0) {   /* ---- MIDI: copy notes, split straddlers ---- */
            double off = a - cs;   /* comp clip-relative offset */
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

    /* Set duration + proxy path from the source so the session model (export,
     * save/load, hit-testing) is self-consistent. The UI pre-generates the
     * proxy and passes it via the proxy_path member; if absent, fall back to
     * the source duration via the decoder (used by tests / save-load). */
    if (cl->video->proxy_path[0]) {
        cl->video->duration = wb_video_proxy_duration(cl->video->proxy_path);
    }
    if (cl->video->duration <= 0.0) {
        wb_video_decoder *d = wb_video_decoder_open(source_path);
        if (d) { cl->video->duration = wb_video_decoder_get_duration(d); wb_video_decoder_close(d); }
    }
    /* Default clip length = full source duration (UI may trim later). */
    cl->length = cl->video->duration;

    /* Grow the session's total song length (in samples) to cover this clip,
     * so wb_engine_render_session (which requires s->length > 0) spans it. */
    if (cl->video->duration > 0.0) {
        double clip_end_samples = (cl->start + cl->video->duration) * WB_SAMPLE_RATE;
        if (clip_end_samples > s->length) s->length = clip_end_samples;
    }
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
