/* wb_session.c — session model: build, destroy, and a demo-song builder.
 * A session is the editable arrangement: tracks, clips, notes. The engine
 * consumes it at render time. This file also owns the .wbus project
 * serialization (load/save) — plain text, human-readable.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "wbus.h"

/* ---- ownership helpers ------------------------------------------------ */
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
    free(s);
}

/* ---- project save/load (text .wbus format) ---------------------------- */
int wb_session_save(wb_session *s, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "wbus 1\nname %s\nbpm %.2f\ntime %d %d\nlength %.0f\ntracks %u\n",
            s->name, s->bpm, s->time_sig_num, s->time_sig_den, s->length, s->track_count);
    for (uint32_t t = 0; t < s->track_count; t++) {
        wb_track *tr = &s->tracks[t];
        fprintf(f, "track %s kind %d vol %.2f pan %.2f mute %d solo %d clips %u\n",
                tr->name, tr->kind, tr->volume, tr->pan, tr->mute, tr->solo, tr->clip_count);
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            wb_clip *cl = &tr->clips[c];
            fprintf(f, " clip type %d start %.0f len %.0f notes %u\n",
                    cl->type, cl->start, cl->length, cl->note_count);
            for (uint32_t k = 0; k < cl->note_count; k++)
                fprintf(f, "  n %.0f %.0f %u %u\n",
                        cl->notes[k].start, cl->notes[k].dur,
                        cl->notes[k].pitch, cl->notes[k].vel);
        }
    }
    fclose(f);
    return 0;
}

/* ---- demo song --------------------------------------------------------- */
/* Build a simple demo session: a synth lead line over a bass. This is what
 * the first render/playback exercise uses to prove the engine works. */
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
    s->track_count = 2;
    s->tracks = calloc(2, sizeof(wb_track));

    /* Track 0: lead synth (C major arpeggio) */
    wb_track *lead = &s->tracks[0];
    snprintf(lead->name, sizeof(lead->name), "Lead");
    lead->kind = 0;
    lead->volume = 0.8f;
    lead->pan = 0.0f;
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

    return s;
}
