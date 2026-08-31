/* wb_project_templates.c — project templates for quick session creation.
 *
 * R078 H10: Pre-built track layouts for common workflows.
 *
 * Pure C11. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "wbus.h"

typedef struct {
    const char *name;
    const char *description;
    int bpm;
    int time_sig_num;
    int time_sig_den;
} template_info_t;

static const template_info_t templates[] = {
    {"Empty",          "Single stereo master track",                            120, 4, 4},
    {"Songwriting",    "Piano, bass, 2 vocal tracks + drums bus",             120, 4, 4},
    {"Podcast",        "Host + guest audio, music bus, voice polish",          120, 4, 4},
    {"Beat Making",    "4 MIDI tracks (drums/bass/chords/lead) + 2 buses",     140, 4, 4},
    {"Orchestral",     "16 MIDI tracks in folder sections",                     90, 4, 4},
    {"Rock Band",      "2 guitars, bass, drums, 2 vocals + buses",            120, 4, 4},
    {"Film Scoring",   "32 tracks with orchestral folders and cue markers",     60, 4, 4},
    {"EDM",            "8 MIDI + 4 audio, extensive bus routing",              128, 4, 4},
    {"YouTube Video",  "1 video, 2 audio (voice+music), captions",             120, 4, 4},
    {"Music Video",    "2 video, 4 audio, beat-sync markers",                  120, 4, 4},
};

int wb_proj_template_count(void) {
    return sizeof(templates) / sizeof(templates[0]);
}

const char* wb_proj_template_get_name(int template_id) {
    if (template_id < 0 || template_id >= wb_proj_template_count()) return NULL;
    return templates[template_id].name;
}

const char* wb_proj_template_get_description(int template_id) {
    if (template_id < 0 || template_id >= wb_proj_template_count()) return NULL;
    return templates[template_id].description;
}

/* Helper: add a MIDI instrument track */
static void add_midi_track(wb_session *s, const char *name) {
    wb_session_add_track(s, name, WB_TRACK_KIND_INSTR);
}

/* Helper: add an audio track */
static void add_audio_track(wb_session *s, const char *name) {
    wb_session_add_track(s, name, WB_TRACK_KIND_AUDIO);
}

/* Helper: add a bus track */
static int add_bus(wb_session *s, const char *name) {
    return wb_session_add_track(s, name, WB_TRACK_KIND_BUS) ? 0 : -1;
}

/* Helper: route a track to a bus */
static void route_to_bus(wb_session *s, int track_idx, int bus_idx) {
    if (track_idx >= 0 && (uint32_t)track_idx < s->track_count)
        s->tracks[track_idx].route = bus_idx;
}

int wb_proj_template_apply(wb_session *session, int template_id) {
    if (!session || template_id < 0 || template_id >= wb_proj_template_count())
        return -1;

    const template_info_t *t = &templates[template_id];
    session->bpm = t->bpm;
    session->time_sig_num = t->time_sig_num;
    session->time_sig_den = t->time_sig_den;

    switch (template_id) {
    case 0: /* Empty — already has nothing, just ensure clean state */
        break;

    case 1: { /* Songwriting */
        add_midi_track(session, "Piano");
        add_midi_track(session, "Bass");
        add_audio_track(session, "Vocal L");
        add_audio_track(session, "Vocal R");
        int drums_bus = wb_session_add_track(session, "Drums Bus", WB_TRACK_KIND_BUS);
        (void)drums_bus;
        break;
    }

    case 2: { /* Podcast */
        add_audio_track(session, "Host");
        add_audio_track(session, "Guest");
        int music_bus = wb_session_add_track(session, "Music Bus", WB_TRACK_KIND_BUS);
        (void)music_bus;
        break;
    }

    case 3: { /* Beat Making */
        add_midi_track(session, "Drums");
        add_midi_track(session, "Bass");
        add_midi_track(session, "Chords");
        add_midi_track(session, "Lead");
        int drums_bus = wb_session_add_track(session, "Drums Bus", WB_TRACK_KIND_BUS);
        int instr_bus = wb_session_add_track(session, "Instr Bus", WB_TRACK_KIND_BUS);
        /* Route drums to drums bus, others to instruments bus */
        route_to_bus(session, 0, drums_bus);
        route_to_bus(session, 1, instr_bus);
        route_to_bus(session, 2, instr_bus);
        route_to_bus(session, 3, instr_bus);
        break;
    }

    case 4: { /* Orchestral */
        add_midi_track(session, "Violin I");
        add_midi_track(session, "Violin II");
        add_midi_track(session, "Viola");
        add_midi_track(session, "Cello");
        add_midi_track(session, "Double Bass");
        add_midi_track(session, "Flute");
        add_midi_track(session, "Oboe");
        add_midi_track(session, "Clarinet");
        add_midi_track(session, "Bassoon");
        add_midi_track(session, "Trumpet");
        add_midi_track(session, "French Horn");
        add_midi_track(session, "Trombone");
        add_midi_track(session, "Tuba");
        add_midi_track(session, "Timpani");
        add_midi_track(session, "Snare");
        add_midi_track(session, "Cymbals");
        int strings_bus = wb_session_add_track(session, "Strings Bus", WB_TRACK_KIND_BUS);
        int brass_bus = wb_session_add_track(session, "Brass Bus", WB_TRACK_KIND_BUS);
        int wood_bus = wb_session_add_track(session, "Woodwinds Bus", WB_TRACK_KIND_BUS);
        int perc_bus = wb_session_add_track(session, "Percussion Bus", WB_TRACK_KIND_BUS);
        for (int i = 0; i <= 4; i++) route_to_bus(session, i, strings_bus);
        for (int i = 5; i <= 8; i++) route_to_bus(session, i, wood_bus);
        for (int i = 9; i <= 12; i++) route_to_bus(session, i, brass_bus);
        for (int i = 13; i <= 15; i++) route_to_bus(session, i, perc_bus);
        break;
    }

    case 5: { /* Rock Band */
        add_audio_track(session, "Guitar L");
        add_audio_track(session, "Guitar R");
        add_midi_track(session, "Bass");
        add_midi_track(session, "Drums");
        add_audio_track(session, "Lead Vocal");
        add_audio_track(session, "Backing Vocal");
        int guitar_bus = wb_session_add_track(session, "Guitars Bus", WB_TRACK_KIND_BUS);
        int vocal_bus = wb_session_add_track(session, "Vocals Bus", WB_TRACK_KIND_BUS);
        int drums_bus = wb_session_add_track(session, "Drums Bus", WB_TRACK_KIND_BUS);
        route_to_bus(session, 0, guitar_bus);
        route_to_bus(session, 1, guitar_bus);
        route_to_bus(session, 2, -1); /* bass to master */
        route_to_bus(session, 3, drums_bus);
        route_to_bus(session, 4, vocal_bus);
        route_to_bus(session, 5, vocal_bus);
        break;
    }

    case 6: { /* Film Scoring */
        /* Strings section */
        for (int i = 0; i < 8; i++) {
            char name[32];
            snprintf(name, sizeof(name), "Strings %d", i + 1);
            add_midi_track(session, name);
        }
        /* Brass section */
        for (int i = 0; i < 6; i++) {
            char name[32];
            snprintf(name, sizeof(name), "Brass %d", i + 1);
            add_midi_track(session, name);
        }
        /* Woodwinds */
        for (int i = 0; i < 5; i++) {
            char name[32];
            snprintf(name, sizeof(name), "Woodwind %d", i + 1);
            add_midi_track(session, name);
        }
        /* Percussion */
        for (int i = 0; i < 6; i++) {
            char name[32];
            snprintf(name, sizeof(name), "Percussion %d", i + 1);
            add_midi_track(session, name);
        }
        /* Synth/FX */
        for (int i = 0; i < 7; i++) {
            char name[32];
            snprintf(name, sizeof(name), "Synth %d", i + 1);
            add_midi_track(session, name);
        }
        int strings_bus = wb_session_add_track(session, "Strings Bus", WB_TRACK_KIND_BUS);
        int brass_bus = wb_session_add_track(session, "Brass Bus", WB_TRACK_KIND_BUS);
        int wood_bus = wb_session_add_track(session, "Woodwinds Bus", WB_TRACK_KIND_BUS);
        int perc_bus = wb_session_add_track(session, "Percussion Bus", WB_TRACK_KIND_BUS);
        int synth_bus = wb_session_add_track(session, "Synth Bus", WB_TRACK_KIND_BUS);
        for (int i = 0; i < 8; i++) route_to_bus(session, i, strings_bus);
        for (int i = 8; i < 14; i++) route_to_bus(session, i, brass_bus);
        for (int i = 14; i < 19; i++) route_to_bus(session, i, wood_bus);
        for (int i = 19; i < 25; i++) route_to_bus(session, i, perc_bus);
        for (int i = 25; i < 32; i++) route_to_bus(session, i, synth_bus);
        /* Add cue markers */
        wb_session_add_marker(session, 0, "Intro", 1);
        wb_session_add_marker(session, WB_SAMPLE_RATE * 30, "Scene 1", 1);
        wb_session_add_marker(session, WB_SAMPLE_RATE * 60, "Chorus", 1);
        wb_session_add_marker(session, WB_SAMPLE_RATE * 90, "Scene 2", 1);
        wb_session_add_marker(session, WB_SAMPLE_RATE * 120, "Outro", 1);
        break;
    }

    case 7: { /* EDM */
        add_midi_track(session, "Kick");
        add_midi_track(session, "Snare");
        add_midi_track(session, "Hi-Hats");
        add_midi_track(session, "Bass");
        add_midi_track(session, "Chords");
        add_midi_track(session, "Lead");
        add_midi_track(session, "Pad");
        add_midi_track(session, "Arp");
        add_audio_track(session, "Vocal Chop");
        add_audio_track(session, "FX Riser");
        add_audio_track(session, "FX Impact");
        add_audio_track(session, "Ambience");
        int drums_bus = wb_session_add_track(session, "Drums Bus", WB_TRACK_KIND_BUS);
        int bass_bus = wb_session_add_track(session, "Bass Bus", WB_TRACK_KIND_BUS);
        int leads_bus = wb_session_add_track(session, "Leads Bus", WB_TRACK_KIND_BUS);
        int fx_bus = wb_session_add_track(session, "FX Bus", WB_TRACK_KIND_BUS);
        route_to_bus(session, 0, drums_bus);
        route_to_bus(session, 1, drums_bus);
        route_to_bus(session, 2, drums_bus);
        route_to_bus(session, 3, bass_bus);
        route_to_bus(session, 4, leads_bus);
        route_to_bus(session, 5, leads_bus);
        route_to_bus(session, 6, leads_bus);
        route_to_bus(session, 7, leads_bus);
        route_to_bus(session, 8, leads_bus);
        route_to_bus(session, 9, fx_bus);
        route_to_bus(session, 10, fx_bus);
        route_to_bus(session, 11, fx_bus);
        break;
    }

    case 8: { /* YouTube Video */
        wb_session_add_track(session, "Video", WB_TRACK_KIND_VIDEO);
        add_audio_track(session, "Voice");
        add_audio_track(session, "Music");
        add_audio_track(session, "Captions");
        break;
    }

    case 9: { /* Music Video */
        wb_session_add_track(session, "Video A", WB_TRACK_KIND_VIDEO);
        wb_session_add_track(session, "Video B", WB_TRACK_KIND_VIDEO);
        add_audio_track(session, "Song");
        add_audio_track(session, "Vocals");
        add_audio_track(session, "SFX");
        add_audio_track(session, "Voiceover");
        /* Beat-sync markers */
        double beat_dur = WB_SAMPLE_RATE * 60.0 / session->bpm;
        for (int i = 0; i < 32; i++) {
            char label[32];
            snprintf(label, sizeof(label), "Beat %d", i + 1);
            wb_session_add_marker(session, (double)i * beat_dur, label, 0);
        }
        break;
    }

    default:
        return -1;
    }

    return 0;
}
