/* wb_daw.c — Big Mac DAW application.
 * SDL2 window with three panels:
 *   - Transport bar (play/stop/seek, BPM, time readout)
 *   - Arrangement view (tracks as lanes, MIDI notes as bars on a grid)
 *   - Mixer strip (per-track volume/pan/mute/solo)
 * SDL2 audio pulls from the engine on the RT thread.
 *
 * UI keeps its own copy of transport state for drawing; commands are sent
 * to the engine through the lock-free queue. Keyboard: Space=play/stop,
 * Left/Right = seek, 1-9 = track volume down/up (for now demo).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>

#include "wbus.h"
#include "wbus_backend.h"
#include "wb_internal.h"

/* ---- geometry --------------------------------------------------------- */
#define WIN_W 1280
#define WIN_H 720
#define TRANSPORT_H 64
#define MIXER_W 220
#define HEADER_H 28

#define GRID_L 48        /* left margin for track name labels */
#define ARRANG_W (WIN_W - MIXER_W - GRID_L)

typedef struct app {
    wb_engine *engine;
    wb_session *session;
    wb_transport t;      /* UI-local transport for drawing */
    wb_tuner *tuner;     /* recursive learn/fix loop */

    SDL_Window *win;
    SDL_Renderer *ren;
} app;

static int running = 1;

/* ---- color helpers ----------------------------------------------------- */
static void set_color(SDL_Renderer *r, int c, int track) {
    (void)track;
    switch (c) {
    case 0: SDL_SetRenderDrawColor(r, 24, 26, 30, 255); break;   /* bg */
    case 1: SDL_SetRenderDrawColor(r, 48, 52, 60, 255); break;   /* panel */
    case 2: SDL_SetRenderDrawColor(r, 90, 140, 220, 255); break; /* accent */
    case 3: SDL_SetRenderDrawColor(r, 60, 66, 76, 255); break;   /* lane */
    case 4: SDL_SetRenderDrawColor(r, 200, 80, 80, 255); break;  /* playhead */
    case 5: SDL_SetRenderDrawColor(r, 220, 220, 225, 255); break;/* text */
    case 6: SDL_SetRenderDrawColor(r, 150, 120, 60, 255); break; /* note bar */
    default: SDL_SetRenderDrawColor(r, 255, 0, 255, 255); break;
    }
}

/* sample pos -> x in arrangement */
static int arr_x(double sample_pos) {
    double px_per_sec = ARRANG_W / 12.0; /* 12 seconds visible */
    return GRID_L + (int)((sample_pos / WB_SAMPLE_RATE) * px_per_sec);
}

/* ---- rendering --------------------------------------------------------- */
static void draw_transport(app *a) {
    SDL_Rect r = { 0, 0, WIN_W, TRANSPORT_H };
    set_color(a->ren, 1, 0); SDL_RenderFillRect(a->ren, &r);

    /* play/stop button */
    SDL_Rect btn = { 12, 12, 40, 40 };
    set_color(a->ren, a->t.playing ? 4 : 2, 0);
    SDL_RenderFillRect(a->ren, &btn);

    /* BPM */
    char bpm[64];
    snprintf(bpm, sizeof(bpm), "BPM %.1f", a->t.bpm);
    /* time readout m:ss.t */
    double sec = a->t.song_pos / WB_SAMPLE_RATE;
    int m = (int)(sec / 60), s = (int)(sec) % 60, cs = (int)((sec - (int)sec) * 100);
    char timebuf[64];
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d.%02d", m, s, cs);

    /* loss readout (tuner's latest sense/act/measure/compare/fix cycle) */
    double loss = a->tuner ? wb_tuner_last_loss(a->tuner) : 0;
    char lossbuf[32];
    snprintf(lossbuf, sizeof(lossbuf), "loss=%.4f", loss);

    set_color(a->ren, 5, 0);
    /* simple text via SDL_ttf would be ideal; we approximate with a title
     * bar (fonts pulled later). For now, fill a "hud" strip on the right. */
    SDL_Rect hud = { 60, 12, 260, 40 };
    set_color(a->ren, 0, 0); SDL_RenderFillRect(a->ren, &hud);
    set_color(a->ren, 2, 0);
    SDL_RenderDrawRect(a->ren, &hud);
    (void)bpm; (void)timebuf;
}

static void draw_arrangement(app *a) {
    /* arrangement area */
    SDL_Rect arr = { GRID_L, TRANSPORT_H, ARRANG_W, WIN_H - TRANSPORT_H - HEADER_H };
    set_color(a->ren, 0, 0); SDL_RenderFillRect(a->ren, &arr);

    if (!a->session) return;
    int n = (int)a->session->track_count;
    double track_h = (double)(arr.h) / (n > 0 ? n : 1);
    double px_per_sec = ARRANG_W / 12.0;

    /* grid lines every beat */
    double bps = a->t.bpm / 60.0;
    set_color(a->ren, 3, 0);
    for (int b = 0; b < (int)(12.0 * bps); b++) {
        int x = GRID_L + (int)(b / bps * px_per_sec);
        SDL_RenderDrawLine(a->ren, x, TRANSPORT_H, x, TRANSPORT_H + arr.h);
    }

    /* track lanes + notes */
    for (int ti = 0; ti < n; ti++) {
        wb_track *tr = &a->session->tracks[ti];
        int y = TRANSPORT_H + (int)(ti * track_h);
        SDL_Rect lane = { GRID_L, y, ARRANG_W, (int)track_h };
        set_color(a->ren, (ti % 2) ? 3 : 1, ti);
        SDL_RenderFillRect(a->ren, &lane);

        /* track label in left gutter */
        SDL_Rect lbl = { 2, y, GRID_L - 4, (int)track_h };
        set_color(a->ren, 5, ti);
        /* (text rendering via a font is added next; gutter shows a bar) */
        set_color(a->ren, 6, ti);
        SDL_RenderFillRect(a->ren, &lbl);

        /* clips -> note bars */
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            wb_clip *cl = &tr->clips[c];
            for (uint32_t k = 0; k < cl->note_count; k++) {
                wb_note *nt = &cl->notes[k];
                double s = cl->start + nt->start;
                double dur = nt->dur;
                int x = arr_x(s);
                int w = (int)((dur / WB_SAMPLE_RATE) * px_per_sec);
                if (w < 3) w = 3;
                int ny = y + 4 + ((nt->pitch % 12) * (int)(track_h - 8) / 12);
                SDL_Rect bar = { x, ny, w, (int)(track_h - 8) / 12 - 1 };
                set_color(a->ren, 6, ti);
                SDL_RenderFillRect(a->ren, &bar);
            }
        }
    }

    /* playhead */
    int px = arr_x(a->t.song_pos);
    set_color(a->ren, 4, 0);
    SDL_RenderDrawLine(a->ren, px, TRANSPORT_H, px, TRANSPORT_H + arr.h);
}

static void draw_mixer(app *a) {
    int mx = WIN_W - MIXER_W;
    SDL_Rect m = { mx, TRANSPORT_H, MIXER_W, WIN_H - TRANSPORT_H - HEADER_H };
    set_color(a->ren, 1, 0); SDL_RenderFillRect(a->ren, &m);

    if (!a->session) return;
    int n = (int)a->session->track_count;
    double strip_w = (double)MIXER_W / (n > 0 ? n : 1);
    double fader_h = (double)(m.h - 40);

    for (int ti = 0; ti < n; ti++) {
        wb_track *tr = &a->session->tracks[ti];
        int x = mx + (int)(ti * strip_w);
        int sw = (int)strip_w - 4;

        /* strip bg */
        SDL_Rect strip = { x, TRANSPORT_H + 4, sw, m.h - 8 };
        set_color(a->ren, 0, ti); SDL_RenderFillRect(a->ren, &strip);

        /* volume fader track (vertical) */
        int fx = x + sw / 2 - 4;
        int fy_top = TRANSPORT_H + 30;
        int fy_bot = TRANSPORT_H + 30 + (int)fader_h;
        SDL_Rect ftr = { fx, fy_top, 8, (int)fader_h };
        set_color(a->ren, 3, ti); SDL_RenderFillRect(a->ren, &ftr);

        /* fader position (vol 0..1) */
        int fy = fy_bot - (int)(tr->volume * fader_h);
        SDL_Rect knob = { fx - 4, fy - 5, 16, 10 };
        set_color(a->ren, tr->mute ? 4 : 2, ti);
        SDL_RenderFillRect(a->ren, &knob);

        /* mute/solo buttons */
        SDL_Rect mute = { x + 2, fy_bot + 6, sw/2 - 2, 16 };
        SDL_Rect solo = { x + sw/2, fy_bot + 6, sw/2, 16 };
        set_color(a->ren, tr->mute ? 4 : 3, ti); SDL_RenderFillRect(a->ren, &mute);
        set_color(a->ren, tr->solo ? 2 : 3, ti); SDL_RenderFillRect(a->ren, &solo);
    }
}

static void render(app *a) {
    /* pull fresh transport state for drawing (the engine's RT copy) */
    wb_engine_get_transport(a->engine, &a->t);

    set_color(a->ren, 0, 0);
    SDL_RenderClear(a->ren);
    draw_transport(a);
    draw_arrangement(a);
    draw_mixer(a);
    SDL_RenderPresent(a->ren);
}

/* ---- input ------------------------------------------------------------ */
static void handle_key(app *a, SDL_Keycode k) {
    switch (k) {
    case SDLK_SPACE:
        if (a->t.playing) wb_engine_stop(a->engine);
        else wb_engine_play(a->engine);
        break;
    case SDLK_RIGHT:
        wb_engine_seek(a->engine, a->t.song_pos + WB_SAMPLE_RATE / 4);
        break;
    case SDLK_LEFT:
        wb_engine_seek(a->engine, a->t.song_pos - WB_SAMPLE_RATE / 4);
        break;
    case SDLK_ESCAPE: case SDLK_q:
        running = 0;
        break;
    default: break;
    }
}

/* ---- main -------------------------------------------------------------- */
int main(int argc, char **argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    app *a = calloc(1, sizeof(*a));
    a->engine = wb_engine_create();
    a->session = wb_session_demo();
    wb_engine_set_session(a->engine, a->session);

    a->win = SDL_CreateWindow("Big Mac DAW", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    if (!a->win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    /* Try accelerated (GPU) first; fall back to software for weak hardware. */
    a->ren = SDL_CreateRenderer(a->win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!a->ren) {
        a->ren = SDL_CreateRenderer(a->win, -1, SDL_RENDERER_SOFTWARE);
        if (!a->ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    }

    /* recursive self-improvement loop: learns & tunes DSP params at idle */
    a->tuner = wb_tuner_create(a->engine);
    wb_tuner_start(a->tuner);

    /* realtime audio backend */
    wb_backend *audio = wb_backend_coreaudio_create(a->engine, WB_SAMPLE_RATE);
    if (!audio) { fprintf(stderr, "audio backend failed: %s\n", SDL_GetError()); }
    else wb_backend_start(audio);

    SDL_Event ev;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_KEYDOWN) handle_key(a, ev.key.keysym.sym);
        }
        render(a);
        SDL_Delay(16);   /* ~60fps */
    }

    if (audio) wb_backend_destroy(audio);
    if (a->tuner) { wb_tuner_stop(a->tuner); wb_tuner_destroy(a->tuner); }
    SDL_DestroyRenderer(a->ren);
    SDL_DestroyWindow(a->win);
    wb_engine_destroy(a->engine);
    wb_session_destroy(a->session);
    free(a);
    SDL_Quit();
    return 0;
}
