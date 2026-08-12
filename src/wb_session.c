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
    return 0;
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

    return s;
}
