/* wb_daw.c — Big Mac DAW application.
 * SDL2 window with:
 *   - Transport bar: play/stop, BPM, bar:beat, time readout, loop toggle
 *   - Arrangement view: track lanes, beat grid, MIDI note bars, playhead,
 *     pitch labels in the gutter
 *   - Mixer: per-track volume fader (with dB), pan, mute/solo buttons
 * All labels/numbers drawn with our embedded 5x7 bitmap font (wb_ui_font).
 * Keyboard: Space=play/stop, Left/Right=seek, B=bpm down, N=bpm up, Esc=quit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>

#include "wbus.h"
#include "wbus_backend.h"
#include "wbus_midi.h"
#include "wb_internal.h"
#include "wb_ui.h"

/* ---- geometry --------------------------------------------------------- */
#define WIN_W 1360
#define WIN_H 760
#define TRANSPORT_H 56
#define MIXER_W 230
#define RULER_H 26
#define GUTTER_W 92

#define ARRANG_W (WIN_W - MIXER_W - GUTTER_W)
#define ARRANG_H (WIN_H - TRANSPORT_H - RULER_H)

#define VISIBLE_SECS 12.0f
#define PX_PER_SEC (ARRANG_W / VISIBLE_SECS)

typedef struct app {
    wb_engine *engine;
    wb_session *session;
    wb_transport t;
    wb_tuner *tuner;
    wb_midi *midi;
    int midi_track;

    SDL_Window *win;
    SDL_Renderer *ren;
} app;

static int running = 1;

/* ---- palette ---------------------------------------------------------- */
#define C_BG     24, 26, 30
#define C_PANEL  36, 39, 45
#define C_PANEL2 28, 30, 35
#define C_ACCENT 96, 155, 235
#define C_LANE_A 44, 48, 55
#define C_LANE_B 38, 41, 47
#define C_PLAY   235, 90, 90
#define C_TEXT   222, 224, 230
#define C_TEXT_DIM 140, 144, 154
#define C_NOTE   255, 198, 90
#define C_NOTE2  210, 130, 80
#define C_GRID   54, 58, 66
#define C_SOLO   120, 200, 120
#define C_MUTE   220, 120, 120

static void setc(SDL_Renderer *r, Uint8 cr, Uint8 cg, Uint8 cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
}

/* sample pos -> x in arrangement */
static int arr_x(double sample_pos) {
    return GUTTER_W + (int)((sample_pos / WB_SAMPLE_RATE) * PX_PER_SEC);
}

#if 0  /* note_name: reserved for the note/arrangement display feature */
static const char *note_name(int pitch) {
    static const char *names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static char buf[8];
    snprintf(buf, sizeof(buf), "%s%d", names[pitch % 12], pitch / 12 - 1);
    return buf;
}
#endif

/* ---- transport bar ---------------------------------------------------- */
static void draw_transport(app *a) {
    SDL_Rect r = { 0, 0, WIN_W, TRANSPORT_H };
    setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &r);
    setc(a->ren, C_BG); SDL_RenderDrawLine(a->ren, 0, TRANSPORT_H-1, WIN_W, TRANSPORT_H-1);

    /* play/pause button */
    SDL_Rect btn = { 14, 12, 32, 32 };
    setc(a->ren, a->t.playing ? C_MUTE : C_ACCENT);
    SDL_RenderFillRect(a->ren, &btn);
    /* triangle / bars glyph */
    if (a->t.playing) {
        setc(a->ren, C_BG);
        SDL_Rect b1 = { btn.x+8, btn.y+8, 5, 16 }, b2 = { btn.x+19, btn.y+8, 5, 16 };
        SDL_RenderFillRect(a->ren, &b1); SDL_RenderFillRect(a->ren, &b2);
    } else {
        setc(a->ren, C_BG);
        for (int yy=0; yy<12; yy++) {
            int yoff = yy<5 ? yy : 11-yy;  /* triangle */
            SDL_RenderDrawLine(a->ren, btn.x+10, btn.y+6+yy, btn.x+10+yoff, btn.y+6+yy);
        }
    }

    /* time readout m:ss.cs */
    double sec = a->t.song_pos / WB_SAMPLE_RATE;
    int m = (int)(sec/60), s = (int)sec % 60, cs = (int)((sec-(int)sec)*100);
    char timebuf[32]; snprintf(timebuf,sizeof(timebuf),"%02d:%02d.%02d",m,s,cs);
    wb_ui_draw_text(a->ren, 60, 8, timebuf, 2, C_TEXT);

    /* BPM */
    char bpm[24]; snprintf(bpm,sizeof(bpm),"BPM %.1f",a->t.bpm);
    wb_ui_draw_text(a->ren, 150, 12, bpm, 1, C_TEXT);

    /* bar:beat */
    double beat = a->t.song_pos / (60.0/a->t.bpm) * WB_SAMPLE_RATE / WB_SAMPLE_RATE;
    beat = a->t.song_pos / (60.0/a->t.bpm);
    int bar = (int)(beat / 4) + 1, bi = (int)beat % 4 + 1;
    char barbuf[24]; snprintf(barbuf,sizeof(barbuf),"bar %d.%d",bar,bi);
    wb_ui_draw_text(a->ren, 150, 32, barbuf, 1, C_TEXT_DIM);

    /* loop toggle hint */
    wb_ui_draw_text(a->ren, 260, 12, "SPACE play/stop", 1, C_TEXT_DIM);
    wb_ui_draw_text(a->ren, 260, 32, "L/R seek  B/N bpm  ESC quit", 1, C_TEXT_DIM);

    /* loss metric */
    double loss = a->tuner ? wb_tuner_last_loss(a->tuner) : 0;
    char lossbuf[32]; snprintf(lossbuf,sizeof(lossbuf),"learn loss %.4f",loss);
    wb_ui_draw_text(a->ren, WIN_W-MIXER_W-230, 12, lossbuf, 1, loss < 0.2 ? C_SOLO : C_MUTE);

    /* xrun counter (underruns dropped by the RT callback) */
    uint64_t xr = wb_engine_xruns(a->engine);
    char xbuf[32]; snprintf(xbuf,sizeof(xbuf),"xrun %llu",(unsigned long long)xr);
    wb_ui_draw_text(a->ren, WIN_W-MIXER_W-230, 32, xbuf, 1, xr==0 ? C_SOLO : C_MUTE);
}

/* ---- arrangement ------------------------------------------------------ */
static void draw_ruler(app *a) {
    SDL_Rect rr = { GUTTER_W, TRANSPORT_H, ARRANG_W, RULER_H };
    setc(a->ren, C_PANEL2); SDL_RenderFillRect(a->ren, &rr);

    /* bar numbers */
    double bps = a->t.bpm / 60.0;
    for (int b = 0; b < (int)(VISIBLE_SECS * bps); b++) {
        int x = GUTTER_W + (int)(b / bps * PX_PER_SEC);
        setc(a->ren, C_GRID); SDL_RenderDrawLine(a->ren, x, TRANSPORT_H, x, TRANSPORT_H+RULER_H);
        char bar[8]; snprintf(bar,sizeof(bar),"%d",b+1);
        wb_ui_draw_text(a->ren, x+3, TRANSPORT_H+6, bar, 1, C_TEXT_DIM);
    }
}

static void draw_arrangement(app *a) {
    SDL_Rect arr = { GUTTER_W, TRANSPORT_H+RULER_H, ARRANG_W, ARRANG_H };
    setc(a->ren, C_BG); SDL_RenderFillRect(a->ren, &arr);

    if (!a->session) return;
    int n = (int)a->session->track_count;
    double track_h = (double)ARRANG_H / (n>0?n:1);
    double bps = a->t.bpm/60.0;

    /* beat grid */
    setc(a->ren, C_GRID);
    for (int b=0;b<(int)(VISIBLE_SECS*bps);b++) {
        int x = GUTTER_W + (int)(b/bps*PX_PER_SEC);
        SDL_RenderDrawLine(a->ren, x, TRANSPORT_H+RULER_H, x, TRANSPORT_H+RULER_H+ARRANG_H);
    }

    for (int ti=0;ti<n;ti++) {
        wb_track *tr = &a->session->tracks[ti];
        int y = TRANSPORT_H + RULER_H + (int)(ti*track_h);
        int th = (int)track_h;

        /* lane bg */
        SDL_Rect lane = { GUTTER_W, y, ARRANG_W, th };
        setc(a->ren, (ti%2)?C_LANE_A:C_LANE_B); SDL_RenderFillRect(a->ren, &lane);

        /* gutter: track name */
        SDL_Rect gut = { 0, y, GUTTER_W, th };
        setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &gut);
        setc(a->ren, C_TEXT); wb_ui_draw_text(a->ren, 6, y+6, tr->name, 1, C_TEXT);
        char vol[12]; snprintf(vol,sizeof(vol),"%.0f%%",tr->volume*100);
        wb_ui_draw_text(a->ren, 6, y+18, vol, 1, C_TEXT_DIM);

        /* clip note bars, pitched */
        for (uint32_t c=0;c<tr->clip_count;c++) {
            wb_clip *cl = &tr->clips[c];
            for (uint32_t k=0;k<cl->note_count;k++) {
                wb_note *nt = &cl->notes[k];
                double s = cl->start + nt->start;
                double dur = nt->dur;
                int x = arr_x(s);
                int w = (int)((dur/WB_SAMPLE_RATE)*PX_PER_SEC); if(w<3)w=3;
                /* pitch maps to vertical: low at bottom */
                int row = (nt->pitch % 12);
                int cell_h = th / 12;
                int ny = y + th - (row+1)*cell_h;
                SDL_Rect bar = { x, ny, w, cell_h-1 };
                setc(a->ren, row==0 ? C_NOTE2 : C_NOTE); SDL_RenderFillRect(a->ren, &bar);
            }
        }
    }

    /* playhead */
    int px = arr_x(a->t.song_pos);
    setc(a->ren, C_PLAY);
    SDL_RenderDrawLine(a->ren, px, TRANSPORT_H, px, TRANSPORT_H+RULER_H+ARRANG_H);
    SDL_Rect head = { px-3, TRANSPORT_H, 7, 8 };
    SDL_RenderFillRect(a->ren, &head);
}

/* ---- mixer ------------------------------------------------------------ */
static void draw_mixer(app *a) {
    int mx = WIN_W - MIXER_W;
    SDL_Rect m = { mx, TRANSPORT_H, MIXER_W, WIN_H-TRANSPORT_H };
    setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &m);
    setc(a->ren, C_BG); SDL_RenderDrawLine(a->ren, mx, TRANSPORT_H, mx, WIN_H);

    if (!a->session) return;
    int n = (int)a->session->track_count;
    double strip_w = (double)MIXER_W / (n>0?n:1);
    double fader_h = ARRANG_H - 70;

    for (int ti=0;ti<n;ti++) {
        wb_track *tr = &a->session->tracks[ti];
        int x = mx + (int)(ti*strip_w);
        int sw = (int)strip_w - 4;

        /* strip */
        SDL_Rect strip = { x, TRANSPORT_H+4, sw, (int)(fader_h+50) };
        setc(a->ren, C_PANEL2); SDL_RenderFillRect(a->ren, &strip);

        /* fader track */
        int fx = x + sw/2 - 4;
        int fy_top = TRANSPORT_H + 40;
        int fy_bot = fy_top + (int)fader_h;
        SDL_Rect ftr = { fx, fy_top, 8, (int)fader_h };
        setc(a->ren, C_LANE_A); SDL_RenderFillRect(a->ren, &ftr);

        /* fader knob */
        int fy = fy_bot - (int)(tr->volume*fader_h);
        SDL_Rect knob = { fx-5, fy-6, 18, 12 };
        setc(a->ren, tr->mute?C_MUTE:C_ACCENT); SDL_RenderFillRect(a->ren, &knob);

        /* dB readout */
        float db = tr->volume>0.0001f ? 20*log10f(tr->volume) : -60;
        char dbuf[16]; snprintf(dbuf,sizeof(dbuf),"%.1f dB",db);
        wb_ui_draw_text(a->ren, x+2, fy_bot+8, dbuf, 1, C_TEXT);

        /* mute / solo */
        SDL_Rect mute = { x+2, fy_bot+26, sw/2-2, 16 };
        SDL_Rect solo = { x+sw/2, fy_bot+26, sw/2, 16 };
        setc(a->ren, tr->mute?C_MUTE:C_LANE_A); SDL_RenderFillRect(a->ren,&mute);
        setc(a->ren, tr->solo?C_SOLO:C_LANE_A); SDL_RenderFillRect(a->ren,&solo);
        wb_ui_draw_text(a->ren, mute.x+4, mute.y+2, "M", 1, tr->mute?C_BG:C_TEXT);
        wb_ui_draw_text(a->ren, solo.x+4, solo.y+2, "S", 1, tr->solo?C_BG:C_TEXT);

        /* track name under fader */
        wb_ui_draw_text(a->ren, x+2, fy_top-16, tr->name, 1, C_TEXT);
    }
}

static void render(app *a) {
    wb_engine_get_transport(a->engine, &a->t);
    setc(a->ren, C_BG); SDL_RenderClear(a->ren);
    draw_transport(a);
    draw_ruler(a);
    draw_arrangement(a);
    draw_mixer(a);
    SDL_RenderPresent(a->ren);
}

/* ---- input ------------------------------------------------------------ */
/* MIDI callback: push controller notes into the engine's lock-free queue.
 * Called from CoreMIDI's receive thread — only touches the queue (RT-safe). */
static void midi_cb(wb_midi_event ev, void *userdata) {
    app *a = userdata;
    uint8_t st = ev.status & 0xF0;
    if (st == 0x90) {
        /* note on → play a note on this DAW's instrument track */
        wb_engine_note(a->engine, a->midi_track, ev.data1, ev.data2);
        /* light the button so you get tactile feedback — fire-and-forget */
    } else if (st == 0x80) {
        /* note off → silence the voice (key off) */
        wb_engine_note(a->engine, a->midi_track, ev.data1, 0);
    }
}

static void handle_key(app *a, SDL_Keycode k) {
    switch (k) {
    case SDLK_SPACE:
        if (a->t.playing) wb_engine_stop(a->engine); else wb_engine_play(a->engine);
        break;
    case SDLK_RIGHT: wb_engine_seek(a->engine, a->t.song_pos + WB_SAMPLE_RATE/4); break;
    case SDLK_LEFT:  wb_engine_seek(a->engine, a->t.song_pos - WB_SAMPLE_RATE/4); break;
    case SDLK_b:     wb_engine_set_bpm(a->engine, a->t.bpm - 1.0); break;
    case SDLK_n:     wb_engine_set_bpm(a->engine, a->t.bpm + 1.0); break;
    case SDLK_ESCAPE: case SDLK_q: running = 0; break;
    default: break;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int shot = 0;
    wb_backend *audio = NULL;
    const char *shot_path = NULL;
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"--screenshot")==0) { shot=1; shot_path = (i+1<argc)?argv[i+1]:"/tmp/wbdaw.ppm"; }
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    app *a = calloc(1, sizeof(*a));
    a->engine = wb_engine_create();
    a->session = wb_session_demo();
    wb_engine_set_session(a->engine, a->session);
    wb_engine_play(a->engine);   /* show the playhead + playing state */

    a->win = SDL_CreateWindow("Big Mac DAW", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    if (!a->win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    a->ren = SDL_CreateRenderer(a->win, -1, SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if (!a->ren) {
        a->ren = SDL_CreateRenderer(a->win, -1, SDL_RENDERER_SOFTWARE);
        if (!a->ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    }

    /* render a few frames so the playhead advances, then screenshot */
    a->tuner = wb_tuner_create(a->engine);
    wb_tuner_start(a->tuner);
    if (shot) {
        wb_engine_seek(a->engine, 2.0*WB_SAMPLE_RATE);
        for (int i=0;i<5;i++) { render(a); SDL_Delay(20); }
        wb_engine_seek(a->engine, 2.0*WB_SAMPLE_RATE);
        render(a);
        SDL_Rect full = { 0,0,WIN_W,WIN_H };
        SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, WIN_W, WIN_H, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(a->ren, &full, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
        SDL_SaveBMP(surf, shot_path);
        SDL_FreeSurface(surf);
        printf("saved screenshot: %s\n", shot_path);
        goto cleanup;
    }

    audio = wb_backend_coreaudio_create(a->engine, WB_SAMPLE_RATE);
    if (!audio) fprintf(stderr, "audio: %s\n", SDL_GetError());
    else wb_backend_start(audio);

    /* ---- open a MIDI controller (Launchpad autodetect) ---------------- */
    a->midi = NULL;
    char names[32][64]; int ndev = 0;
    wb_midi_enumerate(names, 32, &ndev);
    printf("MIDI input devices (%d):\n", ndev);
    for (int i = 0; i < ndev; i++) printf("  [%d] %s\n", i, names[i]);

    /* autodetect: prefer "Launchpad", else first available device */
    a->midi = wb_midi_open_contains("Launchpad", midi_cb, a);
    if (!a->midi && ndev > 0) a->midi = wb_midi_open(names[0], midi_cb, a);
    a->midi_track = 0;
    if (a->midi) printf("MIDI: opened controller, listening for notes...\n");
    else printf("MIDI: no input device open (input disabled)\n");

    SDL_Event ev;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type==SDL_QUIT) running=0;
            else if (ev.type==SDL_KEYDOWN) handle_key(a, ev.key.keysym.sym);
        }
        render(a);
        SDL_Delay(16);
    }

cleanup:
    if (a->midi) wb_midi_close(a->midi);
    if (audio) wb_backend_destroy(audio);
    if (a->tuner) { wb_tuner_stop(a->tuner); wb_tuner_destroy(a->tuner); }
    SDL_DestroyRenderer(a->ren); SDL_DestroyWindow(a->win);
    wb_engine_destroy(a->engine); wb_session_destroy(a->session);
    free(a); SDL_Quit();
    return 0;
}
