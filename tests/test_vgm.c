/* test_vgm.c — VGMusic MIDI parser tests (R096) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#define CHECK(c, m) do { if (c) { p++; printf("  PASS: %s\n", m); } \
                         else { f++; printf("  FAIL: %s\n", m); } } while(0)

/* Create a minimal valid MIDI file in memory */
static uint8_t *make_test_midi(int *out_size) {
    /* Format 0, 1 track, 480 ticks/qn */
    /* Track: tempo event + note on + note off + end of track */
    static uint8_t midi[] = {
        /* MThd header */
        'M', 'T', 'h', 'd',
        0, 0, 0, 6,       /* header length */
        0, 0,             /* format 0 */
        0, 1,             /* 1 track */
        0x01, 0xE0,       /* 480 ticks/qn */
        /* MTrk header */
        'M', 'T', 'r', 'k',
        0, 0, 0, 20,      /* track length (will be updated) */
        /* Delta 0: Tempo meta (120 BPM = 500000 us/qn) */
        0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,
        /* Delta 0: Program change (piano) */
        0x00, 0xC0, 0x00,
        /* Delta 0: Note on (middle C, vel 100) */
        0x00, 0x90, 0x3C, 0x64,
        /* Delta 480 (1 quarter): Note off */
        0x83, 0x60, 0x80, 0x3C, 0x00,
        /* Delta 0: End of track */
        0x00, 0xFF, 0x2F, 0x00
    };
    int size = sizeof(midi);
    *out_size = size;
    uint8_t *buf = (uint8_t *)malloc(size);
    memcpy(buf, midi, size);
    return buf;
}

int main(void) {
    int p = 0, f = 0;

    printf("=== VGMusic MIDI Parser (R096) ===\n\n");

    /* ---- MIDI Parsing ---- */
    printf("--- MIDI Parsing ---\n");
    int size;
    uint8_t *midi_data = make_test_midi(&size);
    fprintf(stderr, "MIDI data size: %d\n", size);

    midi_file *midi = wb_midi_parse(midi_data, size);
    fprintf(stderr, "midi = %p\n", (void*)midi);
    CHECK(midi != NULL, "parse: valid MIDI parsed");
    CHECK(midi->header.format == 0, "parse: format 0");
    CHECK(midi->header.n_tracks == 1, "parse: 1 track");
    CHECK(midi->header.ticks_per_qn == 480, "parse: 480 ticks/qn");
    CHECK(midi->n_tracks == 1, "parse: n_tracks = 1");

    /* Check events were parsed */
    CHECK(midi->tracks[0].n_events > 0, "parse: events found");

    /* Check tempo */
    CHECK(midi->tempo_bpm > 110 && midi->tempo_bpm < 130, "parse: tempo ~120 BPM");

    /* Debug: print all events */
    fprintf(stderr, "Events: %d\n", midi->tracks[0].n_events);
    for (int e = 0; e < midi->tracks[0].n_events; e++) {
        fprintf(stderr, "  event[%d]: type=0x%02x d1=%d d2=%d ch=%d\n",
                e, midi->tracks[0].events[e].type,
                midi->tracks[0].events[e].data1,
                midi->tracks[0].events[e].data2,
                midi->tracks[0].events[e].channel);
    }

    /* Check for note events (type is 0x90 for note on, 0x80 for note off) */
    int has_note_on = 0, has_note_off = 0;
    for (int e = 0; e < midi->tracks[0].n_events; e++) {
        if (midi->tracks[0].events[e].type == 0x90 && midi->tracks[0].events[e].data2 > 0) has_note_on = 1;
        if (midi->tracks[0].events[e].type == 0x80) has_note_off = 1;
    }
    CHECK(has_note_on, "parse: note on event found");
    CHECK(has_note_off, "parse: note off event found");

    wb_midi_free(midi);
    CHECK(1, "midi freed");

    /* ---- Invalid MIDI ---- */
    printf("\n--- Invalid MIDI ---\n");
    uint8_t bad[] = {0, 1, 2, 3, 4, 5, 6, 7};
    midi_file *bad_midi = wb_midi_parse(bad, 8);
    CHECK(bad_midi == NULL, "parse: invalid data rejected");

    midi_file *null_midi = wb_midi_parse(NULL, 0);
    CHECK(null_midi == NULL, "parse: NULL rejected");

    wb_midi_free(NULL);
    CHECK(1, "free NULL doesn't crash");

    /* ---- VGMusic Scraper Helpers ---- */
    printf("\n--- VGMusic Helpers ---\n");
    const char *html = "<a href=\"song1.mid\">Song 1</a>"
                        "<a href=\"song2.mid\">Song 2</a>"
                        "<a href=\"notmidi.txt\">Text</a>"
                        "<a href=\"song3.mid\">Song 3</a>";
    int count = vgm_count_files(html, (int)strlen(html));
    CHECK(count == 3, "count_files: found 3 MIDI files");

    char name[256];
    vgm_game_name("https://www.vgmusic.com/music/snes/super_mario_world.mid", name, 256);
    CHECK(strcmp(name, "super mario world") == 0, "game_name: underscores → spaces");

    /* ---- Game File Counts ---- */
    printf("\n--- Game File Counts ---\n");
    int smw = vgm_game_file_count("Super Mario World");
    CHECK(smw > 0, "game_count: Super Mario World has files");
    CHECK(smw == 450, "game_count: SMW = 450 files");

    int unknown = vgm_game_file_count("Some Obscure Game");
    CHECK(unknown == 50, "game_count: unknown game = default 50");

    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    vgm_count_files(NULL, 0);
    vgm_game_name(NULL, NULL, 0);
    CHECK(1, "NULL inputs don't crash");

    printf("\n=== Results: %d/%d passed ===\n", p, p + f);

    free(midi_data);
    return f > 0 ? 1 : 0;
}
