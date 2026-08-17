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
#include "wbus_vst3.h"
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

    /* ---- UI state (interactive arrangement) ---------------- ------------ */
    int selected_track;      /* -1 = none */
    int dragging_clip;       /* -1 = none, else clip index on selected_track */
    int drag_start_x;        /* mouse x where drag began */
    double clip_drag_origin; /* timeline pos where drag began */

    /* zoom / scroll (arrangement navigation) */
    double view_start;       /* timeline sample at left edge of arrangement */
    float  visible_secs;     /* seconds visible across the arrangement width */

    /* plugin-parameter editor (per selected track/slot) */
    int param_view;          /* 1 = showing the param panel */
    int param_slot;          /* slot whose params are shown */
    int param_drag;          /* -1 = none, else param index being dragged */
    int param_drag_x;        /* x where the drag began */

    char project_path[512];  /* current .wbus file, "" = unsaved */
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

/* sample pos -> x in arrangement (respects zoom/scroll view window) */
static int arr_x(app *a, double sample_pos) {
    double secs = sample_pos / WB_SAMPLE_RATE;
    double view_secs = a ? a->visible_secs : VISIBLE_SECS;
    if (view_secs <= 0.0) view_secs = VISIBLE_SECS;  /* guard div-by-zero */
    double view0 = (a ? a->view_start : 0) / WB_SAMPLE_RATE;
    double px = GUTTER_W + (secs - view0) * (ARRANG_W / view_secs);
    return (int)px;
}

/* pixels per second for the current view window (zoom-aware). */
static double arr_px_per_sec(app *a) {
    double view_secs = a ? a->visible_secs : VISIBLE_SECS;
    if (view_secs <= 0.0) view_secs = VISIBLE_SECS;
    return ARRANG_W / view_secs;
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
    wb_ui_draw_text(a->ren, 260, 32, "L/R seek  B/N bpm  P=VST3 edit  wheel=zoom", 1, C_TEXT_DIM);

    /* project file name (SOTA: show what's open) */
    {
        char pname[96];
        if (a->project_path[0]) {
            /* basename */
            const char *base = a->project_path;
            const char *slash = strrchr(a->project_path, '/');
            if (slash) base = slash + 1;
            snprintf(pname, sizeof(pname), "%s  ^S to save", base);
        } else {
            snprintf(pname, sizeof(pname), "untitled  ^S to save");
        }
        wb_ui_draw_text(a->ren, 460, 12, pname, 1, C_ACCENT);
    }

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

    /* bar numbers (respect zoom/scroll view window) */
    double bps = a->t.bpm / 60.0;
    double v0 = a->view_start / WB_SAMPLE_RATE;
    double vis = a->visible_secs;
    int b0 = (int)(v0 * bps);
    for (int b = b0; (b - b0) * (1.0/bps) < vis + 1.0/bps; b++) {
        double sec = b / bps;
        int x = GUTTER_W + (int)((sec - v0) * (ARRANG_W / vis));
        if (x < GUTTER_W) continue;
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

    /* beat grid (respect zoom/scroll view window) */
    setc(a->ren, C_GRID);
    double v0 = a->view_start / WB_SAMPLE_RATE;
    double vis = a->visible_secs;
    int b0 = (int)(v0 * bps);
    for (int b=b0; (b - b0) * (1.0/bps) < vis + 1.0/bps; b++) {
        int x = GUTTER_W + (int)(((b/bps) - v0) * (ARRANG_W / vis));
        if (x < GUTTER_W) continue;
        SDL_RenderDrawLine(a->ren, x, TRANSPORT_H+RULER_H, x, TRANSPORT_H+RULER_H+ARRANG_H);
    }

    for (int ti=0;ti<n;ti++) {
        wb_track *tr = &a->session->tracks[ti];
        int y = TRANSPORT_H + RULER_H + (int)(ti*track_h);
        int th = (int)track_h;

        /* lane bg: highlight if this is the selected track */
        SDL_Rect lane = { GUTTER_W, y, ARRANG_W, th };
        if (ti == a->selected_track) { setc(a->ren, 253,160,60); }
        else { setc(a->ren, (ti%2)?C_LANE_A:C_LANE_B); }
        SDL_RenderFillRect(a->ren, &lane);

        /* gutter: track name */
        SDL_Rect gut = { 0, y, GUTTER_W, th };
        setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &gut);
        setc(a->ren, C_TEXT); wb_ui_draw_text(a->ren, 6, y+6, tr->name, 1, C_TEXT);
        char vol[12]; snprintf(vol,sizeof(vol),"%.0f%%",tr->volume*100);
        wb_ui_draw_text(a->ren, 6, y+18, vol, 1, C_TEXT_DIM);

        /* clip contents: notes (MIDI) or waveform (audio) */
        for (uint32_t c=0;c<tr->clip_count;c++) {
            wb_clip *cl = &tr->clips[c];
            if (cl->type == 1 && cl->audio_data && cl->audio_frames > 0) {
                /* audio clip: draw a peak-envelope waveform */
                int wx = arr_x(a, cl->start);
                int ww = (int)((cl->length/WB_SAMPLE_RATE)*arr_px_per_sec(a));
                if (ww < 4) ww = 4;
                SDL_Rect clipbox = { wx, y+4, ww, th-8 };
                if (clipbox.x < GUTTER_W) { int over = GUTTER_W-clipbox.x; clipbox.w -= over; clipbox.x = GUTTER_W; }
                if (clipbox.w <= 0) continue;
                SDL_Rect bg = clipbox;
                setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &bg);
                /* down-sample the waveform into per-pixel columns */
                setc(a->ren, C_NOTE2);
                int cols = clipbox.w;
                uint32_t ch = cl->audio_channels > 0 ? cl->audio_channels : 1;
                for (int px=0; px<cols; px++) {
                    /* sample window for this pixel column */
                    double f0 = (double)px / cols;
                    double f1 = (double)(px+1) / cols;
                    uint32_t s0 = (uint32_t)(f0 * cl->audio_frames);
                    uint32_t s1 = (uint32_t)(f1 * cl->audio_frames);
                    if (s1 <= s0) s1 = s0 + 1;
                    if (s1 > cl->audio_frames) s1 = cl->audio_frames;
                    float peak = 0;
                    for (uint32_t sm = s0; sm < s1; sm++) {
                        float v = cl->audio_data[sm*ch];
                        if (v < 0) v = -v;
                        if (v > peak) peak = v;
                    }
                    if (peak > 1.0f) peak = 1.0f;
                    int amp = (int)(peak * (clipbox.h/2));
                    int mid = clipbox.y + clipbox.h/2;
                    SDL_RenderDrawLine(a->ren, clipbox.x+px, mid-amp, clipbox.x+px, mid+amp);
                }
                /* clip border */
                setc(a->ren, C_GRID);
                SDL_RenderDrawRect(a->ren, &clipbox);
                continue;
            }
            for (uint32_t k=0;k<cl->note_count;k++) {
                wb_note *nt = &cl->notes[k];
                double s = cl->start + nt->start;
                double dur = nt->dur;
                int x = arr_x(a, s);
                int w = (int)((dur/WB_SAMPLE_RATE)*arr_px_per_sec(a)); if(w<3)w=3;
                /* pitch maps to vertical: full lane = PITCH_SPAN semitones, low at bottom */
                int span = 24;  /* 2 octaves visible per lane */
                int row = nt->pitch % span;
                int cell_h = th / span;
                int ny = y + th - (row+1)*cell_h;
                SDL_Rect bar = { x, ny, w, cell_h-1 };
                setc(a->ren, (nt->pitch%12)==0 ? C_NOTE2 : C_NOTE); SDL_RenderFillRect(a->ren, &bar);
            }
        }
    }

    /* playhead */
    int px = arr_x(a, a->t.song_pos);
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

        /* insert-chain readout (shows FX routing for this track) */
        if (ti == a->selected_track) {
            int iy = fy_bot + 46;
            int any = 0;
            for (int s = 0; s < WB_MAX_INSERT_SLOTS; s++) {
                const char *id = tr->inserts[s].id;
                if (!id || !id[0]) continue;
                char chain[64];
                snprintf(chain, sizeof(chain), "  %d:%s%s", s,
                         id, tr->sidechain[s] >= 0 ? "<-" : "");
                wb_ui_draw_text(a->ren, x+2, iy, chain, 1, C_TEXT_DIM);
                iy += 12;
                any = 1;
                if (iy > WIN_H - 12) break;
            }
            if (!any)
                wb_ui_draw_text(a->ren, x+2, iy, "  (no inserts)", 1, C_TEXT_DIM);
        }
    }
}

/* ---- VST3 parameter editor panel -------------------------------------- */
#define PED_X        GUTTER_W
#define PED_Y        (TRANSPORT_H + RULER_H + 8)
#define PED_W        400
#define PED_ROW_H    19
#define PED_TITLE_H  22
#define PED_SLIDER_X (PED_X + 150)
#define PED_SLIDER_W 210
#define PED_MAX_ROWS 18

/* Draw the VST3 parameter editor for the selected track's param_slot.
 * Shows nothing if the slot isn't a VST3 plugin or the editor is off. */
static void draw_param_editor(app *a) {
    if (!a->param_view || !a->session) return;
    int ti = a->selected_track;
    if (ti < 0 || ti >= (int)a->session->track_count) return;
    wb_track *tr = &a->session->tracks[ti];
    const char *id = tr->inserts[a->param_slot].id;
    if (!id || strncmp(id, "vst3:", 5) != 0) {
        /* slot has no VST3 plugin — tell the user how to change slot */
        SDL_Rect panel = { PED_X, PED_Y, PED_W, PED_TITLE_H + 16 };
        setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &panel);
        char msg[80]; snprintf(msg, sizeof(msg), "slot %d: not a VST3 (^/up-down to switch)", a->param_slot);
        wb_ui_draw_text(a->ren, PED_X+8, PED_Y+6, msg, 1, C_TEXT_DIM);
        return;
    }
    void *inst = wb_vst3_slot_get(ti, a->param_slot);
    if (!inst) return;
    int n = wb_vst3_param_count(inst);
    int rows = n < PED_MAX_ROWS ? n : PED_MAX_ROWS;
    int ph = PED_TITLE_H + rows * PED_ROW_H + 8;
    SDL_Rect panel = { PED_X, PED_Y, PED_W, ph };
    setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &panel);
    setc(a->ren, C_BG); SDL_RenderDrawRect(a->ren, &panel);

    char title[96];
    snprintf(title, sizeof(title), "VST3: %s  (slot %d, %d params)", id+5, a->param_slot, n);
    wb_ui_draw_text(a->ren, PED_X+8, PED_Y+5, title, 1, C_ACCENT);

    for (int i = 0; i < rows; i++) {
        int ry = PED_Y + PED_TITLE_H + i * PED_ROW_H;
        char pname[64];
        if (wb_vst3_param_name(inst, i, pname, sizeof(pname)) < 0) pname[0] = '\0';
        wb_ui_draw_text(a->ren, PED_X+8, ry+3, pname, 1, (i==a->param_drag)?C_NOTE:C_TEXT);
        /* slider track */
        SDL_Rect sb = { PED_SLIDER_X, ry+4, PED_SLIDER_W, PED_ROW_H-9 };
        setc(a->ren, C_LANE_A); SDL_RenderFillRect(a->ren, &sb);
        float v = wb_vst3_get_param(inst, i);
        int kx = PED_SLIDER_X + (int)(v * (PED_SLIDER_W-6));
        if (kx < PED_SLIDER_X) kx = PED_SLIDER_X;
        if (kx > PED_SLIDER_X+PED_SLIDER_W-6) kx = PED_SLIDER_X+PED_SLIDER_W-6;
        SDL_Rect knob = { kx, ry+2, 6, PED_ROW_H-5 };
        setc(a->ren, C_NOTE); SDL_RenderFillRect(a->ren, &knob);
        char val[12]; snprintf(val, sizeof(val), "%.2f", v);
        wb_ui_draw_text(a->ren, PED_SLIDER_X+PED_SLIDER_W+8, ry+3, val, 1, C_TEXT_DIM);
    }
}

static void render(app *a) {
    wb_engine_get_transport(a->engine, &a->t);
    setc(a->ren, C_BG); SDL_RenderClear(a->ren);
    draw_transport(a);
    draw_ruler(a);
    draw_arrangement(a);
    draw_mixer(a);
    draw_param_editor(a);
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
        /* light the pad so you get tactile feedback — fire-and-forget.
         * If the incoming note is a Launchpad grid note (0-127), reflect it
         * back as an LED with a green-on, dim-on-release color. */
        if (a->midi) {
            int row = ev.data1 / 16, col = ev.data1 % 16;
            if (row <= 7 && col <= 7)
                wb_launchpad_led(a->midi, row, col, 3);  /* green */
        }
    } else if (st == 0x80) {
        /* note off → silence the voice (key off) + dim the LED */
        wb_engine_note(a->engine, a->midi_track, ev.data1, 0);
        if (a->midi) {
            int row = ev.data1 / 16, col = ev.data1 % 16;
            if (row <= 7 && col <= 7)
                wb_launchpad_led(a->midi, row, col, 0);  /* off */
        }
    }
}

/* ---- project open/save (SOTA .wbus workflow) --------------------------- */
/* Replace the current session with one loaded from `path`. Returns 0 ok,
 * -1 if the load failed (current session is preserved). */
static int load_project(app *a, const char *path) {
    wb_session *s = wb_session_load(path);
    if (!s) {
        fprintf(stderr, "project: failed to load %s\n", path);
        return -1;
    }
    /* swap the session, rebuild the engine runtime, reset selection */
    wb_session *old = a->session;
    a->session = s;
    wb_engine_set_session(a->engine, a->session);
    a->selected_track = -1;
    a->dragging_clip = -1;
    snprintf(a->project_path, sizeof(a->project_path), "%s", path);
    wb_session_destroy(old);
    printf("project: loaded %s (%u tracks)\n", path, s->track_count);
    return 0;
}

/* Save the current session to `path` (defaults to the current project path). */
static int save_project(app *a, const char *path) {
    const char *dst = (path && path[0]) ? path : a->project_path;
    if (!dst || !dst[0]) {
        fprintf(stderr, "project: no save path (use Ctrl+S <file> or --file)\n");
        return -1;
    }
    if (wb_session_save(a->session, dst) != 0) {
        fprintf(stderr, "project: failed to save %s\n", dst);
        return -1;
    }
    if (!a->project_path[0]) snprintf(a->project_path, sizeof(a->project_path), "%s", dst);
    printf("project: saved %s (%u tracks)\n", dst, a->session->track_count);
    return 0;
}

/* ---- arrangement interaction ------------------------------------------- */
/* Convert a screen x in the arrangement to a sample position (clamped to 0).
 * Zoom-aware: inverts the same window math arr_x() uses. */
static double x_to_sample(app *a, int x) {
    if (x < GUTTER_W) x = GUTTER_W;
    int maxx = GUTTER_W + ARRANG_W;
    if (x > maxx) x = maxx;
    double vis = a ? a->visible_secs : VISIBLE_SECS;
    if (vis <= 0.0) vis = VISIBLE_SECS;
    double view0 = a ? a->view_start / WB_SAMPLE_RATE : 0.0;
    double secs = view0 + (double)(x - GUTTER_W) / (ARRANG_W / vis);
    if (secs < 0) secs = 0;
    return secs * WB_SAMPLE_RATE;
}

/* Track index under screen y, or -1. */
static int y_to_track(app *a, int y) {
    int n = a->session ? a->session->track_count : 0;
    if (n == 0) return -1;
    int top = TRANSPORT_H + RULER_H;
    if (y < top || y >= top + ARRANG_H) return -1;
    double track_h = (double)ARRANG_H / n;
    return (int)((y - top) / track_h);
}

/* Convert a screen y within a track lane to a MIDI pitch (2-octave span). */
static int y_to_pitch(app *a, int ti, int y) {
    int n = a->session ? a->session->track_count : 0;
    if (n == 0) return 60;
    int top = TRANSPORT_H + RULER_H;
    double track_h = (double)ARRANG_H / n;
    double span = 24.0;
    double rel = (y - top - ti*track_h) / track_h;   /* 0..1 within lane */
    if (rel < 0) rel = 0; if (rel > 1) rel = 1;
    int row = (int)(rel * span);
    int base = 48;  /* C3 */
    return base + (span - 1 - row);
}

static void handle_mouse(app *a, SDL_MouseButtonEvent b) {
    /* If the VST3 param editor is open and the click lands on a slider row,
     * arm that parameter for dragging (motion handler does the rest). */
    if (a->param_view && b.button == SDL_BUTTON_LEFT) {
        int ti = a->selected_track;
        if (ti >= 0 && ti < (int)a->session->track_count) {
            const char *id = a->session->tracks[ti].inserts[a->param_slot].id;
            void *inst = (id && strncmp(id,"vst3:",5)==0) ? wb_vst3_slot_get(ti, a->param_slot) : NULL;
            if (inst && b.x >= PED_SLIDER_X-4 && b.x <= PED_SLIDER_X+PED_SLIDER_W+4
                && b.y >= PED_Y+PED_TITLE_H && b.y <= PED_Y+PED_TITLE_H+PED_MAX_ROWS*PED_ROW_H) {
                int row = (b.y - (PED_Y+PED_TITLE_H)) / PED_ROW_H;
                int n = wb_vst3_param_count(inst);
                if (row >= 0 && row < n) {
                    a->param_drag = row;
                    /* apply initial value at click x */
                    float v = (float)(b.x - PED_SLIDER_X) / PED_SLIDER_W;
                    if (v<0) v=0; if (v>1) v=1;
                    wb_vst3_set_param(inst, row, v);
                    wb_engine_set_insert_param(a->engine, ti, a->param_slot, row, v);
                    return;
                }
            }
        }
    }
    if (!a->session || b.x < GUTTER_W) return;
    int ti = y_to_track(a, b.y);
    if (ti < 0) return;
    a->selected_track = ti;

    Uint32 btn = b.button;
    if (btn == SDL_BUTTON_LEFT) {
        /* seek playhead to click position (scrub-on-click) */
        double pos = x_to_sample(a, b.x);
        wb_engine_seek(a->engine, pos);
        a->clip_drag_origin = pos;
        /* piano-roll: left-click in an empty lane adds a note (1 beat, mid vel) */
        if (b.y > TRANSPORT_H + RULER_H) {
            int pitch = y_to_pitch(a, ti, b.y);
            double start = pos / WB_SAMPLE_RATE;
            double beat = 60.0 / a->t.bpm;
            wb_session_add_note(&a->session->tracks[ti], start, beat, pitch, 100);
            wb_engine_set_session(a->engine, a->session);  /* rebuild runtime */
        }
    }
    /* right-click toggles mute on the track under cursor OR deletes a note */
    if (btn == SDL_BUTTON_RIGHT) {
        int pitch = y_to_pitch(a, ti, b.y);
        double start = x_to_sample(a, b.x) / WB_SAMPLE_RATE;
        if (wb_session_remove_note(&a->session->tracks[ti], start, pitch) == 0) {
            wb_engine_set_session(a->engine, a->session);
        } else {
            a->session->tracks[ti].mute = !a->session->tracks[ti].mute;
            wb_engine_set_session(a->engine, a->session);
        }
    }
}

/* Total song length in samples (for scroll clamping). Falls back to a
 * generous default if the session length wasn't set. */
static double song_len_samples(app *a) {
    double len = a->session ? a->session->length : 0;
    if (len <= 0) {
        /* derive from longest clip end if length is unset */
        double mx = 30.0 * WB_SAMPLE_RATE;
        if (a->session) {
            for (uint32_t ti = 0; ti < a->session->track_count; ti++) {
                wb_track *tr = &a->session->tracks[ti];
                for (uint32_t c = 0; c < tr->clip_count; c++) {
                    wb_clip *cl = &tr->clips[c];
                    double end = cl->start + cl->length;
                    if (end > mx) mx = end;
                }
            }
        }
        return mx;
    }
    return len;
}

/* Mouse wheel: horizontal scroll (shift+wheel) or vertical scroll;
 * plain wheel zooms the timeline around the cursor. */
static void handle_wheel(app *a, SDL_MouseWheelEvent w) {
    if (!a->session) return;
    int mx = ARRANG_W / 2 + GUTTER_W;  /* zoom anchor: center of arrangement */
    if (w.x != 0 || (SDL_GetModState() & KMOD_SHIFT)) {
        /* horizontal scroll (or shift+wheel) — pan the view */
        double px = (w.x != 0 ? w.x : w.y);
        a->view_start += px * 0.10 * a->visible_secs * WB_SAMPLE_RATE;
    } else {
        /* plain vertical wheel — zoom in/out around the anchor */
        double factor = w.y > 0 ? 0.85 : 1.18;
        double newvis = a->visible_secs * factor;
        if (newvis < 1.0)  newvis = 1.0;
        if (newvis > 600.0) newvis = 600.0;
        /* keep the anchor sample fixed on screen while changing span */
        double anchor_sec = (mx - GUTTER_W) / (ARRANG_W / a->visible_secs);
        double anchor_song = a->view_start / WB_SAMPLE_RATE + anchor_sec;
        a->visible_secs = newvis;
        a->view_start = (anchor_song - (mx - GUTTER_W) / (ARRANG_W / newvis)) * WB_SAMPLE_RATE;
    }
    /* clamp view_start so the window stays within [0, song_len - vis] */
    double max_start = song_len_samples(a) - a->visible_secs * WB_SAMPLE_RATE;
    if (max_start < 0) max_start = 0;
    if (a->view_start < 0) a->view_start = 0;
    if (a->view_start > max_start) a->view_start = max_start;
}

/* Mouse motion: drag a parameter slider when one is armed. */
static void handle_motion(app *a, SDL_MouseMotionEvent m) {
    if (!a->param_view || a->param_drag < 0 || !a->session) return;
    int ti = a->selected_track;
    if (ti < 0) return;
    void *inst = wb_vst3_slot_get(ti, a->param_slot);
    if (!inst) { a->param_drag = -1; return; }
    int sx = PED_SLIDER_X, ex = PED_SLIDER_X + PED_SLIDER_W;
    float v = (float)(m.x - sx) / (ex - sx);
    if (v < 0) v = 0; if (v > 1) v = 1;
    wb_vst3_set_param(inst, a->param_drag, v);
    wb_engine_set_insert_param(a->engine, ti, a->param_slot, a->param_drag, v);
}

static void handle_key(app *a, SDL_Keycode k) {
    Uint32 mod = SDL_GetModState();
    int ctrl = (mod & KMOD_CTRL) != 0;
    switch (k) {
    case SDLK_SPACE:
        if (a->t.playing) wb_engine_stop(a->engine); else wb_engine_play(a->engine);
        break;
    case SDLK_RIGHT: wb_engine_seek(a->engine, a->t.song_pos + WB_SAMPLE_RATE/4); break;
    case SDLK_LEFT:  wb_engine_seek(a->engine, a->t.song_pos - WB_SAMPLE_RATE/4); break;
    case SDLK_b:     wb_engine_set_bpm(a->engine, a->t.bpm - 1.0); break;
    case SDLK_n:
        if (ctrl) {  /* Ctrl+N: new (empty) project */
            wb_session *s = wb_session_create();
            wb_session *old = a->session;
            a->session = s;
            wb_engine_set_session(a->engine, a->session);
            a->selected_track = -1;
            a->project_path[0] = 0;
            wb_session_destroy(old);
            printf("project: new empty session\n");
        } else {
            wb_engine_set_bpm(a->engine, a->t.bpm + 1.0);
        }
        break;
    case SDLK_s:
        if (ctrl) save_project(a, NULL);  /* Ctrl+S: save current project */
        break;
    case SDLK_o:
        if (ctrl) load_project(a, "/tmp/bigmac_proj.wbus"); /* Ctrl+O: open demo project */
        break;
    case SDLK_p:
        /* toggle the VST3 parameter editor for the selected track */
        a->param_view = !a->param_view;
        a->param_drag = -1;
        if (a->selected_track >= 0)
            printf("param editor: %s (track %d, slot %d)\n",
                   a->param_view ? "OPEN" : "closed", a->selected_track, a->param_slot);
        break;
    case SDLK_UP: case SDLK_DOWN:
        /* switch the VST3 slot being edited (only when editor is open) */
        if (a->param_view) {
            int dir = (k == SDLK_UP) ? -1 : 1;
            a->param_slot += dir;
            if (a->param_slot < 0) a->param_slot = 0;
            if (a->param_slot >= WB_MAX_INSERT_SLOTS) a->param_slot = WB_MAX_INSERT_SLOTS - 1;
            a->param_drag = -1;
        }
        break;
    case SDLK_ESCAPE:
        if (a->param_view) { a->param_view = 0; a->param_drag = -1; }
        else { running = 0; }
        break;
    case SDLK_q: running = 0; break;
    default: break;
    }
}

int main(int argc, char **argv) {
    int shot = 0;
    wb_backend *audio = NULL;
    const char *shot_path = NULL;
    const char *file_path = NULL;
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"--screenshot")==0) { shot=1; shot_path = (i+1<argc)?argv[i+1]:"/tmp/wbdaw.ppm"; }
        else if (strcmp(argv[i],"--file")==0 && i+1<argc) { file_path = argv[i+1]; }
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    app *a = calloc(1, sizeof(*a));
    a->selected_track = -1;
    a->dragging_clip = -1;
    a->engine = wb_engine_create();
    if (file_path) {
        /* open a project from disk instead of the demo */
        a->session = wb_session_load(file_path);
        if (!a->session) { fprintf(stderr, "open: failed to load %s\n", file_path); return 1; }
        snprintf(a->project_path, sizeof(a->project_path), "%s", file_path);
        printf("open: loaded %s (%u tracks)\n", file_path, a->session->track_count);
    } else {
        a->session = wb_session_demo();
        a->project_path[0] = 0;
    }
    wb_engine_set_session(a->engine, a->session);
    wb_engine_play(a->engine);   /* show the playhead + playing state */

    /* arrangement view window (zoom/scroll): start fully zoomed-out to the
     * default span. visible_secs MUST be nonzero or arr_x() divides by zero. */
    a->view_start   = 0.0;
    a->visible_secs = VISIBLE_SECS;
    a->param_view   = 0;
    a->param_slot   = 0;
    a->param_drag   = -1;
    a->param_drag_x = 0;

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
    if (a->midi) {
        printf("MIDI: opened controller, listening for notes...\n");
        /* open the matching output destination for LED feedback */
        if (wb_midi_open_output(a->midi, "Launchpad") == 0)
            printf("MIDI: Launchpad LED feedback armed\n");
        else if (wb_midi_open_output(a->midi, NULL) == 0)
            printf("MIDI: controller output armed (generic)\n");
        wb_launchpad_clear(a->midi); /* reset all LEDs */
    }
    else printf("MIDI: no input device open (input disabled)\n");

    SDL_Event ev;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type==SDL_QUIT) running=0;
            else if (ev.type==SDL_KEYDOWN) handle_key(a, ev.key.keysym.sym);
            else if (ev.type==SDL_MOUSEWHEEL) handle_wheel(a, ev.wheel);
            else if (ev.type==SDL_MOUSEMOTION) handle_motion(a, ev.motion);
            else if (ev.type==SDL_MOUSEBUTTONDOWN) handle_mouse(a, ev.button);
            else if (ev.type==SDL_MOUSEBUTTONUP) a->param_drag = -1;
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
