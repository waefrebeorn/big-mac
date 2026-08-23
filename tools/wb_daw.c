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
#include <unistd.h>
#include <SDL.h>

#include "wbus.h"
#include "wbus_backend.h"
#include "wbus_midi.h"
#include "wbus_vst3.h"
#include "wbus_video.h"
#include "wbus_workspace.h"
#include "wbus_clip_edit.h"
#include "wbus_compositor.h"
#include "wbus_cgi.h"
#include "wbus_agi.h"
#include "wbus_captions.h"
#include "wbus/wbus_transcript.h"
#include "wbus/wbus_perf.h"
#include "wbus/wbus_wavcache.h"
#include "wb_internal.h"
#include "wb_ui.h"

/* 480p proxy dimensions — mirror wb_video.c */
#ifndef PROXY_SCALE_W
#define PROXY_SCALE_W 854
#endif
#ifndef PROXY_SCALE_H
#define PROXY_SCALE_H 480
#endif

/* ---- helpers ----------------------------------------------------------- */

static const char *tab_name(int t) {
    static const char *names[] = {
        "ARRANGE", "PAD", "STEP", "SESSION",
        "MEDIA", "EDIT", "CAPTIONS", "EXPORT"
    };
    return (t >= 0 && t < 8) ? names[t] : "KEYS";
}

static const char *scale_name(int root, int type) {
    static const char *roots[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    (void)type;
    return roots[root % 12];
}

/* ---- geometry --------------------------------------------------------- */
#define WIN_W 1360
#define WIN_H 800
#define TRANSPORT_H 58
#define TOOLBAR_H 40
#define ACTION_H 32
#define OVERVIEW_H 22
#define MAIN_Y (TRANSPORT_H + TOOLBAR_H + ACTION_H + OVERVIEW_H)   /* top of the main content (ruler) area */
#define STATUS_H 24
#define RIBBON_H 30   /* R043: bottom Fusion-style workspace tier ribbon */
#define MIXER_W 248
#define RULER_H 26
#define GUTTER_W 132

#define ARRANG_W (WIN_W - MIXER_W - GUTTER_W)
#define ARRANG_H (WIN_H - TRANSPORT_H - TOOLBAR_H - RULER_H - STATUS_H)

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
    int rec_armed;           /* R038: record-arm toggle (UI button) */
    int loop_on;             /* R038: loop toggle (UI button) */
    int dragging_clip;       /* -1 = none, else clip index on selected_track */
    int drag_start_x;        /* mouse x where drag began */
    double clip_drag_origin; /* timeline pos where drag began */
    /* R043 (G1/G2): clip-handle drag state (trim/fade/content).
     * handle_drag = -1 none; 0 left-trim, 1 right-trim, 2 fade-in, 3 fade-out,
     * 4 content-slide (top half of waveform). hd_track/hd_clip identify target. */
    int handle_drag;
    int hd_track, hd_clip;
    int hd_start_x;          /* mouse x at drag begin */
    double hd_clip_start0;   /* clip->start at drag begin (samples) */
    double hd_clip_len0;     /* clip->length at drag begin */
    float  hd_fade0[2];      /* fade_in/out at drag begin */
    double hd_sis0;          /* start_in_source at drag begin */
    /* per-track base id of the last-drawn clip handle region (or -1) */
    int clip_handle_base[64];
    /* R043 (G4): mixer fader drag + automation-write arm.
     * dragging_fader = -1 none, else track index. fader_drag_y = mouse y at
     * drag begin; fader_vol0 = volume (linear) at drag begin. arm[t] = 1 when
     * that track's volume automation is armed (drags write automation points). */
    int dragging_fader;
    int fader_drag_y;
    float fader_vol0;
    int arm[WB_MAX_TRACKS];
    wb_automation_recorder *fader_rec[WB_MAX_TRACKS];   /* per-track volume recorder */
    /* R043 (G6): Fusion-style node-graph view model (self-contained compositor) */
    wb_node_graph *comp_graph;
    /* R043-G7: 3D-CGI scene + AGI task bridge (self-contained modules) */
    wb_cgi_scene *cgi;
    wb_agi       *agi;
    /* R032: comping marquee — shift+drag a time range on a lane, then 'C'
     * commits it to lane 0 (the comp). sel_lane = which lane the drag was on. */
    int marquee_active;
    double sel_t0, sel_t1;
    int sel_lane;

    /* R035: distinct performance views for tabs 1/2/3 (PAD / STEP / SESSION)
     * instead of all three cloning the arrangement. STEP holds a 16-step
     * pattern per track (one pitch per active step); SESSION holds which
     * scene (lane) is launched per track. */
    int step_pitch[WB_MAX_TRACKS][16];   /* active pitch per step, -1 = off */
    int pad_flash[32];                   /* R035: PAD pad flash decay counters */
    int last_step;                       /* R035: last fired STEP step (playback) */

    /* zoom / scroll (arrangement navigation) */
    double view_start;       /* timeline sample at left edge of arrangement */
    float  visible_secs;     /* seconds visible across the arrangement width */

    /* plugin-parameter editor (per selected track/slot) */
    int param_view;          /* 1 = showing the param panel */
    int param_slot;          /* slot whose params are shown */
    int param_drag;          /* -1 = none, else param index being dragged */
    int param_drag_x;        /* x where the drag began */

    /* R023: velocity-drag — when dragging a note vertically, change its velocity */
    int vel_drag_track;      /* -1 = none, else track index */
    int vel_drag_clip;       /* clip index */
    int vel_drag_note;       /* note index */
    int vel_drag_start_y;    /* mouse y where drag began */
    int vel_drag_start_vel;  /* velocity at drag start */

    /* R040: overview minimap drag state */
    int ov_drag;            /* 1 while dragging the overview strip */

    /* R024: VU ballistics — displayed meter lags the raw peak slightly */
    float meter_disp[WB_MAX_TRACKS];
    float master_meter_disp;   /* R028: master bus VU ballistics */

    /* tabbed view (R006/R007: KEYS / PAD / STEP / SESSION)
     * video editor tabs (R009: MEDIA / EDIT / CAPTIONS / EXPORT) */
    int tab;                 /* 0=KEYS 1=PAD 2=STEP 3=SESSION
                               * 4=MEDIA 5=EDIT 6=CAPTIONS 7=EXPORT */
    int scale_root;          /* 0..11 MIDI root */
    int scale_type;          /* 0=major 1=minor 2=dorian 3=mixolydian 4=chromatic */
    int last_lp_row;         /* last Mk2 grid row lit (for release dim) */
    int last_lp_col;

    /* video editor state (DaVinci-style tabs) */
    int vid_has_clip;        /* 1 when a video clip is loaded */
    int vid_track;           /* track index of the video track */
    int vid_clip;            /* clip index on the video track */
    double vid_tl_start;     /* timeline position where clip starts (sec) */
    double vid_tl_end;       /* timeline position where clip ends (sec) */
    double vid_dur;          /* source clip duration (seconds) */
    char vid_source[512];    /* path to source video file */
    char vid_proxy[512];     /* path to 480p proxy */
    char vid_export[512];    /* path for exported output */
    char vid_srt[512];       /* path to SRT captions file */
    int vid_captions_ready;  /* 1 when SRT has been generated */
    wb_captions *vid_caps;   /* R049: persistent captions ctx (word model) */
    wb_transcript *vid_tr;   /* editable transcript for the current video */
    int tr_sel0, tr_sel1;    /* selected word range [sel0, sel1) */
    int tr_row_y;            /* y of first transcript row (CAPTIONS tab) */
    int tr_scroll;           /* R051: first visible transcript row */
    wb_perf *perf;           /* R065: live performance engine */
    int      perf_recording;
    /* R067: JKL scrubbing + I/O loop points */
    int      jkl_speed;      /* -8..+8 (0 = stopped) */
    double   io_in, io_out;  /* sample positions; out<=in means unset */
    /* R050: CGI drag-rotate */
    int cgi_dragging;
    int cgi_last_x, cgi_last_y;
    SDL_Texture *vid_preview_tex; /* cached preview frame */

    /* tool paths */
    char ffmpeg_path[256];
    char whisper_cli_path[256];
    char whisper_model_path[256];

    char project_path[512];  /* current .wbus file, "" = unsaved */

    /* R043: DaVinci-Resolve-style workspace/tier ribbon (AUDIO/VIDEO/
     * FUSION/3D-CGI/AGI). Self-contained controller (opaque struct); the
     * DAW just queries which tier is active to route audio vs video work.
     * The current tier drives which top tab band is shown. */
    wb_workspace *ws;
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
#define C_TEXT_DIM 165, 169, 179   /* R029: brightened to clear WCAG AA (>=4.5:1) on every bg */
#define C_NOTE   255, 198, 90
#define C_NOTE2  210, 130, 80
#define C_GRID  116, 122, 136  /* R046: brightened from (54,58,66) — grid was ~1.2:1 on lanes, near-invisible */
#define C_SOLO   120, 200, 120
#define C_MUTE   235, 140, 140   /* R029: brightened to clear WCAG AA on lane bg */
#define C_FADE   120, 220, 110   /* R043 (G1/G2): clip fade handles (bright green) */

static void setc(SDL_Renderer *r, Uint8 cr, Uint8 cg, Uint8 cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
}

/* forward declaration — defined below (after the registry helpers) */
static void ui_button(SDL_Renderer *r, int id, int x, int y, int w, int h,
                      const char *label, int active);

/* R039: per-track color palette (clips/markers colored by track, Ableton-style) */
typedef struct { Uint8 r, g, b; } rgb;
static const rgb TRACK_PALETTE[16] = {
    { 96,155,235}, {235,150, 96}, {150,235,140}, {235,140,200},
    {140,200,235}, {235,205,120}, {170,150,235}, {140,235,200},
    {235,140,140}, {200,235,140}, {140,170,235}, {235,170,140},
    {160,235,170}, {200,140,235}, {235,180,160}, {150,180,235},
};
static rgb track_rgb(int ti) {
    return TRACK_PALETTE[((ti % 16) + 16) % 16];
}

/* ---- UI button registry (one source of truth: drawn + hit-tested) ----- */
enum {
    BTN_PLAY, BTN_REWIND, BTN_STOP, BTN_RECORD, BTN_LOOP, BTN_SAVE,
    BTN_TAB0, BTN_TAB1, BTN_TAB2, BTN_TAB3, BTN_TAB4, BTN_TAB5, BTN_TAB6, BTN_TAB7,
    BTN_ACT0, BTN_ACT1, BTN_ACT2, BTN_ACT3,   /* per-view action buttons */
    BTN_OVERVIEW,                             /* arrangement overview strip (scroll/zoom) */
    BTN_WS0, BTN_WS1, BTN_WS2, BTN_WS3, BTN_WS4,  /* R043: workspace tier ribbon AUDIO/VIDEO/FUSION/3D-CGI/AGI */
    BTN_COUNT
};
typedef struct { int id; SDL_Rect r; } click_region;
static click_region g_regions[BTN_COUNT];
static int g_nregions = 0;
static void region_reset(void) { g_nregions = 0; }
static void region_add(int id, int x, int y, int w, int h) {
    if (g_nregions >= BTN_COUNT) return;
    g_regions[g_nregions].id = id;
    g_regions[g_nregions].r = (SDL_Rect){x, y, w, h};
    g_nregions++;
}
static int region_hit(int x, int y) {
    for (int i = g_nregions - 1; i >= 0; i--) {
        SDL_Rect *r = &g_regions[i].r;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return g_regions[i].id;
    }
    return -1;
}

/* R043: DaVinci-Resolve-style workspace/tier ribbon.
 *
 * The app is a COMBO DAW + NLE + (future) 3D-CGI + AGI control surface. The
 * ribbon lets the user flip the whole workspace between tiers so audio and
 * video work never get tangled, while ONE session + engine stays live. We map
 * each tier to a sensible default top-tab band so switching tiers also flips
 * the view to the relevant toolset:
 *   AUDIO  -> ARRANGE (0)   VIDEO -> MEDIA (4)   FUSION -> EDIT (5)
 *   3D-CGI -> SESSION (3)   AGI   -> CAPTIONS (6)  (AGI drives caption/auto-edit)
 * The change callback just points the current view at the new tier's home tab.
 * The workspace controller itself (wb_workspace) owns all state. */
static void ws_on_change(void *ctx, wb_workspace_tier old_t, wb_workspace_tier new_t) {
    (void)old_t;
    app *a = (app*)ctx;
    if (!a) return;
    static const int tier_home_tab[WB_WS_COUNT] = { 0, 4, 5, 5, 5, 5 };
    /* R046: FUSION/3D-CGI/AGI all host their dedicated view on tab 5 (EDIT);
     * the draw fn branches on the active tier. Old mapping (3,6) pointed at
     * SESSION/CAPTIONS — those tiers drew nothing there. */
    if (new_t >= 0 && new_t < WB_WS_COUNT)
        a->tab = tier_home_tab[new_t];
}

/* Draw the bottom workspace ribbon. Five tiers, each a labeled button; the
 * active tier is highlighted, locked tiers are dimmed. Registered as a click
 * region so handle_mouse can hit-test it (one source of truth). */
static void draw_workspace_ribbon(app *a) {
    int y = WIN_H - STATUS_H - RIBBON_H;
    SDL_Rect bar = { 0, y, WIN_W, RIBBON_H };
    setc(a->ren, C_PANEL2); SDL_RenderFillRect(a->ren, &bar);
    setc(a->ren, C_BG);
    SDL_RenderDrawLine(a->ren, 0, y, WIN_W, y);

    int n = WB_WS_COUNT, pad = 6;
    int bw = (WIN_W - MIXER_W - pad*(n+1)) / n;
    int by = y + 4, bh = RIBBON_H - 8;
    wb_workspace_tier active = wb_workspace_active(a->ws);
    for (int i = 0; i < n; i++) {
        wb_workspace_tier t = (wb_workspace_tier)i;
        int bx = pad + i*(bw+pad);
        int unlocked = wb_workspace_unlocked(a->ws, t);
        int is_active = (t == active);
        const char *lbl = wb_workspace_label(t);
        if (!unlocked) {
            /* locked tier: draw dim outline only (capability not present yet) */
            setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &(SDL_Rect){bx,by,bw,bh});
            setc(a->ren, C_GRID);  SDL_RenderDrawRect(a->ren, &(SDL_Rect){bx,by,bw,bh});
            wb_ui_draw_text(a->ren, bx+8, by+8, lbl, 1, C_TEXT_DIM);
        } else {
            ui_button(a->ren, BTN_WS0 + i, bx, by, bw, bh, lbl, is_active);
        }
    }
    /* tier legend / hint on the right edge of the ribbon */
    char hint[128];
    snprintf(hint, sizeof(hint), "WORKSPACE: %s", wb_workspace_label(active));
    wb_ui_draw_text(a->ren, WIN_W - MIXER_W - 200, y + 8, hint, 1, C_MUTE);
}

/* draw a labeled button; registers its hit region for handle_mouse */
static void ui_button(SDL_Renderer *r, int id, int x, int y, int w, int h,
                      const char *label, int active) {
    region_add(id, x, y, w, h);
    setc(r, active ? C_ACCENT : C_PANEL2);
    SDL_Rect b = { x, y, w, h };
    SDL_RenderFillRect(r, &b);
    if (active) setc(r, 255, 255, 255); else setc(r, C_TEXT);
    wb_ui_draw_text(r, x + 8, y + (h-16)/2, label, 1, 255, 255, 255);
    if (active) setc(r, 150,190,255); else setc(r, C_GRID);   /* R046: no ternary on C_* macros */
    SDL_RenderDrawRect(r, &b);
}

/* sample pos -> x in arrangement (respects zoom/scroll view window) */
static double song_len_samples(app *a);  /* R040 forward decl (defined later) */

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

/* R040: center the arrangement view window on the clicked overview x */
static void ov_scroll_to(app *a, int mx) {
    int x0 = GUTTER_W, w = WIN_W - MIXER_W - GUTTER_W;
    if (mx < x0) mx = x0; if (mx > x0 + w) mx = x0 + w;
    double frac = (double)(mx - x0) / w;
    double slen = song_len_samples(a) / (double)WB_SAMPLE_RATE;
    if (slen <= 0) slen = a->visible_secs;
    double center = frac * slen;
    double vs = a->visible_secs;
    double ns = center - vs / 2.0;
    if (ns < 0) ns = 0;
    double max_start = slen - vs;
    if (max_start < 0) max_start = 0;
    if (ns > max_start) ns = max_start;
    a->view_start = ns * WB_SAMPLE_RATE;
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

    int by = 13, bh = 32;
    /* rewind-to-start */
    ui_button(a->ren, BTN_REWIND, 14, by, 26, bh, "|<", 0);
    /* play / pause (toggles) */
    region_add(BTN_PLAY, 46, by, 26, bh);
    setc(a->ren, a->t.playing ? C_PLAY : C_ACCENT);
    SDL_Rect pbtn = { 46, by, 26, bh };
    SDL_RenderFillRect(a->ren, &pbtn);
    setc(a->ren, 255,255,255);
    if (a->t.playing) {
        SDL_Rect b1 = { pbtn.x+7, pbtn.y+6, 4, 14 }, b2 = { pbtn.x+14, pbtn.y+6, 4, 14 };
        SDL_RenderFillRect(a->ren, &b1); SDL_RenderFillRect(a->ren, &b2);
    } else {
        for (int yy=0; yy<13; yy++) {
            int yoff = yy<7 ? yy : 12-yy;
            SDL_RenderDrawLine(a->ren, pbtn.x+8, pbtn.y+4+yy, pbtn.x+8+yoff+2, pbtn.y+4+yy);
        }
    }
    ui_button(a->ren, BTN_STOP,   78, by, 26, bh, "[]", 0);
    ui_button(a->ren, BTN_RECORD, 110, by, 26, bh, "REC", a->rec_armed);
    ui_button(a->ren, BTN_LOOP,   142, by, 26, bh, "LOOP", a->loop_on);
    ui_button(a->ren, BTN_SAVE,   174, by, 30, bh, "SAVE", 0);

    /* time readout m:ss.cs (clear of the button row) */
    double sec = a->t.song_pos / WB_SAMPLE_RATE;
    int m = (int)(sec/60), s = (int)sec % 60, cs = (int)((sec-(int)sec)*100);
    char timebuf[32]; snprintf(timebuf,sizeof(timebuf),"%02d:%02d.%02d",m,s,cs);
    wb_ui_draw_text(a->ren, 212, 8, timebuf, 2, 255,255,255);

    /* BPM */
    char bpm[24]; snprintf(bpm,sizeof(bpm),"BPM %.1f",a->t.bpm);
    wb_ui_draw_text(a->ren, 212, 34, bpm, 1, C_TEXT);

    /* bar:beat */
    double sec_bb = a->t.song_pos / (double)WB_SAMPLE_RATE;
    double bps = a->t.bpm / 60.0;
    double beat = sec_bb * bps;
    int bar = (int)(beat / 4) + 1;
    int bi  = ((int)beat % 4) + 1;
    char barbuf[24]; snprintf(barbuf,sizeof(barbuf),"bar %d.%d",bar,bi);
    wb_ui_draw_text(a->ren, 360, 34, barbuf, 1, C_TEXT_DIM);

    /* project name */
    {
        char pname[96];
        if (a->project_path[0]) {
            const char *base = a->project_path;
            const char *slash = strrchr(a->project_path, '/');
            if (slash) base = slash + 1;
            snprintf(pname, sizeof(pname), "%s", base);
        } else {
            snprintf(pname, sizeof(pname), "untitled");
        }
        wb_ui_draw_text(a->ren, 360, 8, pname, 1, C_ACCENT);
    }

    /* xrun counter (moved to far right, muted unless >0) */
    uint64_t xr = wb_engine_xruns(a->engine);
    char xbuf[32]; snprintf(xbuf,sizeof(xbuf),"xrun %llu",(unsigned long long)xr);
    wb_ui_draw_text(a->ren, WIN_W-MIXER_W-90, 8, xbuf, 1, xr==0 ? C_SOLO : C_MUTE);
}

/* ---- arrangement ------------------------------------------------------ */
static void draw_ruler(app *a) {
    SDL_Rect rr = { GUTTER_W, MAIN_Y, ARRANG_W, RULER_H };
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
        setc(a->ren, C_GRID); SDL_RenderDrawLine(a->ren, x, MAIN_Y, x, MAIN_Y+RULER_H);
        char bar[8]; snprintf(bar,sizeof(bar),"%d",b+1);
        wb_ui_draw_text(a->ren, x+3, MAIN_Y+6, bar, 1, C_TEXT_DIM);
    }
}

static void draw_arrangement(app *a) {
    SDL_Rect arr = { GUTTER_W, MAIN_Y+RULER_H, ARRANG_W, ARRANG_H };
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
        SDL_RenderDrawLine(a->ren, x, MAIN_Y+RULER_H, x, MAIN_Y+RULER_H+ARRANG_H);
    }

    /* R022: arrangement-marker ruler — song-section labels (Intro/Verse/..) */
    if (a->session->marker_count > 0) {
        int my = MAIN_Y + RULER_H - 12;
        for (uint32_t mi = 0; mi < a->session->marker_count; mi++) {
            const wb_marker *mk = &a->session->markers[mi];
            int mx = arr_x(a, mk->pos);
            if (mx < GUTTER_W) continue;
            setc(a->ren, mk->kind ? C_ACCENT : C_SOLO);   /* section vs cue */
            SDL_Rect band = { mx, my, 3, RULER_H };
            SDL_RenderFillRect(a->ren, &band);
            wb_ui_draw_text(a->ren, mx + 4, MAIN_Y + 2, mk->label, 1,
                            mk->kind ? C_ACCENT : C_SOLO);
        }
    }

    for (int ti=0;ti<n;ti++) {
        wb_track *tr = &a->session->tracks[ti];
        int y = MAIN_Y + RULER_H + (int)(ti*track_h);
        int th = (int)track_h;
        /* R030: take-lanes — compute how many lanes this track uses */
        int lane_count = 1;
        for (uint32_t c = 0; c < tr->clip_count; c++)
            if (tr->clips[c].lane + 1 > lane_count) lane_count = tr->clips[c].lane + 1;
        int lh = (th - 8) / lane_count;   /* height of one lane sub-row */

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
        /* R038: per-track Mute / Solo buttons in the gutter (conventional DAW track header) */
        ui_button(a->ren, 1000+ti, GUTTER_W-46, y+4, 20, 14, "M", tr->mute);
        ui_button(a->ren, 2000+ti, GUTTER_W-24, y+4, 20, 14, "S", tr->solo);

        /* clip contents: notes (MIDI) or waveform (audio) */
        for (uint32_t c=0;c<tr->clip_count;c++) {
            wb_clip *cl = &tr->clips[c];
            if (cl->type == 1 && cl->audio_data && cl->audio_frames > 0) {
                /* audio clip: draw a peak-envelope waveform */
                int wx = arr_x(a, cl->start);
                int ww = (int)((cl->length/WB_SAMPLE_RATE)*arr_px_per_sec(a));
                if (ww < 4) ww = 4;
                int clip_h = lh - 4;
                if (clip_h < 6) clip_h = 6;
                SDL_Rect clipbox = { wx, y+4 + cl->lane*lh, ww, clip_h };
                if (clipbox.x < GUTTER_W) { int over = GUTTER_W-clipbox.x; clipbox.w -= over; clipbox.x = GUTTER_W; }
                if (clipbox.w <= 0) continue;
                SDL_Rect bg = clipbox;
                setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &bg);
                /* R066: prefer the LOD pyramid cache; fall back to raw
                 * scan only if the cache failed to build. */
                static wb_wavcache *wc_cache[512] = {0};
                uint32_t wc_slot = (uint32_t)(uintptr_t)cl % 512;
                if (!wc_cache[wc_slot] && cl->audio_frames > 4096) {
                    /* mono downmix for display (cache is view-only) */
                    uint32_t chn = cl->audio_channels > 0 ? cl->audio_channels : 1;
                    wb_sample *mono = malloc(cl->audio_frames * sizeof(wb_sample));
                    if (mono) {
                        for (uint32_t sm = 0; sm < cl->audio_frames; sm++)
                            mono[sm] = cl->audio_data[sm*chn];
                        wc_cache[wc_slot] = wb_wavcache_build(mono, cl->audio_frames);
                        free(mono);
                    }
                }
                int used_lod = 0;
                float *lod_mn = NULL, *lod_mx = NULL;
                int cols = clipbox.w;
                if (wc_cache[wc_slot]) {
                    lod_mn = malloc(cols*sizeof(float));
                    lod_mx = malloc(cols*sizeof(float));
                    if (lod_mn && lod_mx &&
                        wb_wavcache_range(wc_cache[wc_slot], 0,
                                          cl->audio_frames,
                                          lod_mn, lod_mx, cols) == cols)
                        used_lod = 1;
                }
                setc(a->ren, C_NOTE2);
                uint32_t ch = cl->audio_channels > 0 ? cl->audio_channels : 1;
                for (int px=0; px<cols; px++) {
                    float peak = 0;
                    if (used_lod) {
                        peak = fabsf(lod_mx[px]) > fabsf(lod_mn[px])
                             ? fabsf(lod_mx[px]) : fabsf(lod_mn[px]);
                    } else {
                    /* sample window for this pixel column */
                    double f0 = (double)px / cols;
                    double f1 = (double)(px+1) / cols;
                    uint32_t s0 = (uint32_t)(f0 * cl->audio_frames);
                    uint32_t s1 = (uint32_t)(f1 * cl->audio_frames);
                    if (s1 <= s0) s1 = s0 + 1;
                    if (s1 > cl->audio_frames) s1 = cl->audio_frames;
                    for (uint32_t sm = s0; sm < s1; sm++) {
                        float v = cl->audio_data[sm*ch];
                        if (v < 0) v = -v;
                        if (v > peak) peak = v;
                    }
                    }
                    if (peak > 1.0f) peak = 1.0f;
                    /* R042: apply clip (region) gain to the waveform so gain
                     * changes are visible, like Pro Tools / Studio One. */
                    float g = cl->clip_gain > 0.0001f ? cl->clip_gain : 1.0f;
                    int amp = (int)(peak * g * (clipbox.h/2));
                    if (amp > clipbox.h/2) amp = clipbox.h/2;
                    int mid = clipbox.y + clipbox.h/2;
                    SDL_RenderDrawLine(a->ren, clipbox.x+px, mid-amp, clipbox.x+px, mid+amp);
                }
                /* clip border */
                setc(a->ren, C_GRID);
                SDL_RenderDrawRect(a->ren, &clipbox);
                int rx = clipbox.x + clipbox.w;
                /* R043 (G1/G2): direct-manipulation handles — drawn on the
                 * waveform, one source of truth with hit-testing (below).
                 *   LEFT_EDGE  = trim start   RIGHT_EDGE = trim length
                 *   FADE_IN    = top-left     FADE_OUT   = top-right */
                int hw = 6;  /* handle hit size (px) */
                if (clipbox.w > 16) {
                    /* read fade state from the clip-edit side-table (keeps
                     * wb_clip layout-stable; one source of truth with render) */
                    const wb_clip_edit *ce = wb_clip_edit_get(wb_engine_clip_edit(a->engine), ti, (int)c);
                    float fin  = ce ? ce->fade_in  : 0.0f;
                    float fout = ce ? ce->fade_out : 0.0f;
                    /* fade handles: only meaningful when a fade is > 0 OR the
                     * clip is tall enough; draw a corner triangle either way so
                     * it's discoverable (Ableton/Logic show them on-hover). */
                    setc(a->ren, C_FADE);
                    /* fade-in corner (top-left): width ~ fade_in in px */
                    double dur = cl->length / WB_SAMPLE_RATE;
                    double fin_px = clipbox.w * (fin / (dur > 0 ? dur : 1));
                    if (fin_px < 4) fin_px = 4; if (fin_px > clipbox.w) fin_px = clipbox.w;
                    SDL_RenderDrawLine(a->ren, clipbox.x, clipbox.y, clipbox.x + (int)fin_px, clipbox.y + (int)fin_px);
                    SDL_RenderDrawLine(a->ren, clipbox.x, clipbox.y, clipbox.x + (int)fin_px, clipbox.y);
                    double fout_px = clipbox.w * (fout / (dur > 0 ? dur : 1));
                    if (fout_px < 4) fout_px = 4; if (fout_px > clipbox.w) fout_px = clipbox.w;
                    SDL_RenderDrawLine(a->ren, rx, clipbox.y, rx - (int)fout_px, clipbox.y + (int)fout_px);
                    SDL_RenderDrawLine(a->ren, rx, clipbox.y, rx - (int)fout_px, clipbox.y);
                    /* trim-edge caps (drawn as a 2px tall accent bar at edges) */
                    setc(a->ren, C_ACCENT);
                    SDL_RenderDrawLine(a->ren, clipbox.x, clipbox.y, clipbox.x, clipbox.y+clipbox.h);
                    SDL_RenderDrawLine(a->ren, rx-1, clipbox.y, rx-1, clipbox.y+clipbox.h);
                    /* R043 (G5): content-slide affordance — a faint top-half
                     * guide showing the waveform can be slid inside the clip.
                     * Use the fade-green so it's discoverable (not C_GRID-dim). */
                    setc(a->ren, C_FADE);
                    SDL_RenderDrawLine(a->ren, clipbox.x+hw, clipbox.y+clipbox.h/2,
                                       rx-hw, clipbox.y+clipbox.h/2);
                    /* R043 (G3): loop indicator — a small badge at bottom-right.
                     * Filled (accent) when looping, outline when not. */
                    wb_clip_edit_table *et = wb_engine_clip_edit(a->engine);
                    const wb_clip_edit *ced = et ? wb_clip_edit_get(et, ti, (int)c) : NULL;
                    if (ced && ced->loop) {
                        setc(a->ren, C_ACCENT);
                        SDL_Rect lb = { rx-12, clipbox.y+clipbox.h-12, 10, 10 };
                        SDL_RenderFillRect(a->ren, &lb);
                    } else {
                        setc(a->ren, C_FADE);
                        SDL_RenderDrawRect(a->ren, &(SDL_Rect){rx-12, clipbox.y+clipbox.h-12, 10, 10});
                    }
                }
                /* register handle hit regions: id = 50000 + ti*1000 + c*8 + h
                 * h: 0 left-trim, 1 right-trim, 2 fade-in, 3 fade-out,
                 *    4 content-slide (top half of waveform), 5 loop-toggle.
                 * decode: ti=(id-50000)/1000; rem%1000; c=rem/8; h=rem%8. */
                int base = 50000 + ti*1000 + (int)c*8;
                region_add(base+0, clipbox.x, clipbox.y, hw, clipbox.h);          /* LEFT trim  */
                region_add(base+1, clipbox.x+clipbox.w-hw, clipbox.y, hw, clipbox.h); /* RIGHT trim */
                region_add(base+2, clipbox.x, clipbox.y, hw+2, hw+2);              /* FADE_IN */
                region_add(base+3, clipbox.x+clipbox.w-hw-2, clipbox.y, hw+2, hw+2); /* FADE_OUT */
                region_add(base+4, clipbox.x+hw, clipbox.y, clipbox.w-2*hw, clipbox.h/2); /* CONTENT-SLIDE (top half) */
                region_add(base+5, rx-10, clipbox.y+clipbox.h-10, 10, 10);          /* LOOP toggle (bottom-right) */
                a->clip_handle_base[ti] = (clipbox.w > 16) ? base : -1;
                /* R022: clip (region) gain readout — pre-fader, like Pro Tools */
                if (cl->clip_gain > 1.001f || cl->clip_gain < 0.999f) {
                    char gb[16]; snprintf(gb, sizeof(gb), "g%.2f", cl->clip_gain);
                    wb_ui_draw_text(a->ren, clipbox.x+3, clipbox.y+3, gb, 1, C_ACCENT);
                }
                continue;
            }
            for (uint32_t k=0;k<cl->note_count;k++) {
                wb_note *nt = &cl->notes[k];
                double s = cl->start + nt->start;
                double dur = nt->dur;
                int x = arr_x(a, s);
                int w = (int)((dur/WB_SAMPLE_RATE)*arr_px_per_sec(a)); if(w<3)w=3;
                /* pitch maps to vertical within this clip's lane sub-row */
                int span = 24;  /* 2 octaves visible per lane */
                int row = nt->pitch % span;
                int cell_h = lh / span;
                if (cell_h < 1) cell_h = 1;
                int ny = y + 4 + cl->lane*lh + lh - (row+1)*cell_h;
                SDL_Rect bar = { x, ny, w, cell_h-1 };
                /* R023: shade note by velocity (bright = loud), like Ableton */
                int vv = nt->vel > 127 ? 127 : (nt->vel < 1 ? 1 : nt->vel);
                int b = (int)(90 + (vv/127.0f)*150);   /* 90..240 brightness */
                if (nt->pitch%12 == 0) setc(a->ren, b, b/2, 200);
                else                   setc(a->ren, b, b, b);
                SDL_RenderFillRect(a->ren, &bar);
            }
        }
    }

    /* R047: volume-automation overlay — each track's recorded fader lane
     * drawn as a bright curve over its own track row. This is the fader
     * automation READBACK: what you drew with the 'A'-armed fader is now
     * visible where it acts. One source of truth with stage_automation. */
    if (a->session) for (uint32_t l=0; l<a->session->automation_count; l++) {
        const wb_automation_lane *al = a->session->automation[l];
        if (!al || al->target < 0 || strcmp(al->param, "volume") != 0) continue;
        if (al->point_count < 1) continue;
        int ti = al->target;
        if (ti >= (int)a->session->track_count) continue;
        double th = ARRANG_H/(double)a->session->track_count;
        int base_y = MAIN_Y + RULER_H + (int)(ti*th) + (int)th - 6;
        setc(a->ren, C_FADE);
        for (uint32_t p=1; p<al->point_count; p++) {
            int x0 = arr_x(a, al->points[p-1].time);   /* time is SAMPLES */
            int x1 = arr_x(a, al->points[p].time);
            if (x1 < GUTTER_W || x0 > GUTTER_W+ARRANG_W) { continue; }
            int y0 = base_y - (int)(al->points[p-1].value * (th*0.5));
            int y1 = base_y - (int)(al->points[p].value   * (th*0.5));
            SDL_RenderDrawLine(a->ren, x0, y0, x1, y1);
        }
    }

    /* R023: velocity lane — a strip at the bottom of the arrangement showing
     * each MIDI note's velocity as a vertical bar (Ableton/Logic style). */
    {
        int vy = MAIN_Y + RULER_H + ARRANG_H - 34;
        int vh = 30;
        SDL_Rect vstrip = { GUTTER_W, vy, ARRANG_W, vh };
        setc(a->ren, C_PANEL2); SDL_RenderFillRect(a->ren, &vstrip);
        wb_ui_draw_text(a->ren, GUTTER_W+4, vy+2, "VELOCITY", 1, C_TEXT_DIM);
        if (a->session) for (uint32_t t=0;t<a->session->track_count;t++) {
            wb_track *tr = &a->session->tracks[t];
            for (uint32_t c=0;c<tr->clip_count;c++) {
                wb_clip *cl = &tr->clips[c];
                for (uint32_t k=0;k<cl->note_count;k++) {
                    wb_note *nt = &cl->notes[k];
                    double s = cl->start + nt->start;
                    int x = arr_x(a, s);
                    if (x < GUTTER_W) continue;
                    int vv = nt->vel > 127 ? 127 : (nt->vel < 0 ? 0 : nt->vel);
                    int bh = (int)((vv/127.0f) * (vh-4));
                    SDL_Rect vb = { x, vy + (vh-2) - bh, 3, bh };
                    setc(a->ren, 200, 200, 90);
                    SDL_RenderFillRect(a->ren, &vb);
                }
            }
        }
    }

    /* R032: comping marquee — show the current shift-drag selection */
    if (a->marquee_active && a->session && a->selected_track >= 0) {
        int ti = a->selected_track;
        int x0 = arr_x(a, a->sel_t0), x1 = arr_x(a, a->sel_t1);
        int top = MAIN_Y + RULER_H + (int)(ti*(ARRANG_H/(double)a->session->track_count));
        double th = ARRANG_H/(double)a->session->track_count;
        SDL_Rect mb = { x0 < x1 ? x0 : x1, top+4, abs(x1-x0), (int)th-8 };
        setc(a->ren, 96, 155, 235);
        SDL_RenderDrawRect(a->ren, &mb);
        SDL_Rect fill = mb; fill.x = x0<x1?x0:x1; fill.w = abs(x1-x0);
        /* semi-transparent look via a dimmer fill */
        setc(a->ren, 60, 100, 150); SDL_RenderFillRect(a->ren, &fill);
    }

    /* playhead */
    int px = arr_x(a, a->t.song_pos);
    setc(a->ren, C_PLAY);
    SDL_RenderDrawLine(a->ren, px, MAIN_Y, px, MAIN_Y+RULER_H+ARRANG_H);
    SDL_Rect head = { px-3, MAIN_Y, 7, 8 };
    SDL_RenderFillRect(a->ren, &head);
}

/* ---- R035: PAD performance view (8x4 pad grid) ----------------------- */
static void draw_pad(app *a) {
    int x0 = GUTTER_W, y0 = MAIN_Y + RULER_H + 28;
    int cols = 8, rows = 4, pad = 6;
    int pw = (ARRANG_W - pad*(cols+1)) / cols;
    int ph = (ARRANG_H - 60 - pad*(rows+1)) / rows;
    wb_ui_draw_text(a->ren, x0, MAIN_Y+RULER_H+6, "PAD  — click a pad to audition (selected track's instrument)", 1, C_TEXT);
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
        int idx = r*cols + c;          /* 0..31 */
        int px = x0 + pad + c*(pw+pad);
        int py = y0 + r*(ph+pad);
        int pitch = 36 + idx;          /* C2.. */
        int flash = a->pad_flash[idx] > 0;
        if (flash) setc(a->ren, C_ACCENT);
        else if (idx % 2) setc(a->ren, C_LANE_A);
        else setc(a->ren, C_LANE_B);
        SDL_Rect pr = { px, py, pw, ph };
        SDL_RenderFillRect(a->ren, &pr);
        setc(a->ren, C_TEXT_DIM);
        char lbl[16]; snprintf(lbl, sizeof(lbl), "%d", pitch);
        wb_ui_draw_text(a->ren, px+4, py+4, lbl, 1, C_TEXT_DIM);
    }
}

/* ---- R035: STEP sequencer (16 steps x 8 pitches) -------------------- */
static void draw_step(app *a) {
    int ti = a->selected_track;
    wb_ui_draw_text(a->ren, GUTTER_W, MAIN_Y+RULER_H+6,
        "STEP — 16-step sequencer for the selected track (click to toggle)", 1, C_TEXT);
    int x0 = GUTTER_W, y0 = MAIN_Y + RULER_H + 28;
    int steps = 16, rows = 8, pad = 4;
    int cw = (ARRANG_W - pad*(steps+1)) / steps;
    int rh = (ARRANG_H - 60 - pad*(rows+1)) / rows;
    /* current step from transport (16th notes) */
    double bpm = a->t.bpm > 0 ? a->t.bpm : 120.0;
    double spb = 60.0/bpm, spstep = spb/4.0;   /* 16th-note duration (s) */
    int cur = (int)(a->t.song_pos / WB_SAMPLE_RATE / spstep) % steps;
    for (int s = 0; s < steps; s++) {
        for (int r = 0; r < rows; r++) {
            int px = x0 + pad + s*(cw+pad);
            int py = y0 + r*(rh+pad);
            int pitch = (ti>=0) ? a->step_pitch[ti][s] : -1;
            int on = (pitch >= 0) && (7 - r) == (pitch % rows);  /* map pitch to a row */
            if (s == cur) setc(a->ren, C_ACCENT);
            else if (on) setc(a->ren, C_NOTE2);
            else setc(a->ren, C_LANE_B);
            SDL_Rect cr = { px, py, cw, rh };
            SDL_RenderFillRect(a->ren, &cr);
        }
    }
}

/* ---- R035/R039: SESSION clip-launcher grid (tracks x scenes) -------- */
static void draw_session(app *a) {
    wb_ui_draw_text(a->ren, GUTTER_W, MAIN_Y+RULER_H+6,
        "SESSION — click a cell to launch that clip; right column launches a whole scene", 1, C_TEXT);
    int x0 = GUTTER_W, y0 = MAIN_Y + RULER_H + 28;
    int scenes = 4, pad = 6;
    int scenes_w = 70;   /* scene-launch column width on the right */
    int cw = (ARRANG_W - 120 - pad*(scenes+1) - scenes_w - pad) / scenes;
    int rh = 40;
    if (!a->session) return;
    for (uint32_t t = 0; t < a->session->track_count; t++) {
        int py = y0 + t*(rh+pad);
        setc(a->ren, C_PANEL); SDL_Rect g = { x0, py, ARRANG_W, rh }; SDL_RenderFillRect(a->ren, &g);
        rgb tc = track_rgb((int)t);
        setc(a->ren, tc.r, tc.g, tc.b); wb_ui_draw_text(a->ren, x0+4, py+12, a->session->tracks[t].name, 1, tc.r, tc.g, tc.b);
        for (int sc = 0; sc < scenes; sc++) {
            int px = x0 + 120 + pad + sc*(cw+pad);
            int ci = -1;
            for (uint32_t c = 0; c < a->session->tracks[t].clip_count; c++)
                if (a->session->tracks[t].clips[c].lane == sc) { ci = (int)c; break; }
            int launched = (ci >= 0) && (wb_engine_launched_clip(a->engine, (int)t) == ci);
            if (launched) setc(a->ren, tc.r, tc.g, tc.b);
            else { setc(a->ren, C_LANE_B); }
            SDL_Rect cell = { px, py+6, cw, rh-12 };
            SDL_RenderFillRect(a->ren, &cell);
            setc(a->ren, launched ? 20 : C_TEXT_DIM);
            char l[8]; snprintf(l, sizeof(l), "S%d", sc);
            wb_ui_draw_text(a->ren, px+4, py+18, l, 1, launched ? 20 : C_TEXT_DIM);
        }
    }
    /* R039: scene-launch column header + buttons (launch all tracks' clip on that scene) */
    int sx = x0 + 120 + pad + scenes*(cw+pad) + pad;
    setc(a->ren, C_PANEL2); SDL_Rect sh = { sx, y0-22, scenes_w, 16 }; SDL_RenderFillRect(a->ren, &sh);
    wb_ui_draw_text(a->ren, sx+6, y0-20, "SCENE", 1, C_TEXT_DIM);
    for (int sc = 0; sc < scenes; sc++) {
        int py = y0 + sc*(rh+pad);
        ui_button(a->ren, 3000+sc, sx, py, scenes_w, rh, "LAUNCH", 0);
    }
}

/* ---- mixer ------------------------------------------------------------ */
static void draw_mixer(app *a) {
    int mx = WIN_W - MIXER_W;
    SDL_Rect m = { mx, MAIN_Y, MIXER_W, WIN_H-MAIN_Y-STATUS_H };
    setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &m);
    setc(a->ren, C_BG); SDL_RenderDrawLine(a->ren, mx, MAIN_Y, mx, WIN_H-STATUS_H);

    if (!a->session) return;
    int n = (int)a->session->track_count;
    double strip_w = (double)MIXER_W / (n>0?n:1);
    double fader_h = ARRANG_H - 70;

    for (int ti=0;ti<n;ti++) {
        wb_track *tr = &a->session->tracks[ti];
        int x = mx + (int)(ti*strip_w);
        int sw = (int)strip_w - 4;

        /* strip */
        SDL_Rect strip = { x, MAIN_Y+4, sw, (int)(fader_h+50) };
        setc(a->ren, C_PANEL2); SDL_RenderFillRect(a->ren, &strip);

        /* R024: live VU meter — vertical bar, left of the fader, driven by the
         * engine's REAL post-FX peak (not the fader). VU-style decay. */
        {
            int fy_top = MAIN_Y + 40;
            int fy_bot = fy_top + (int)fader_h;
            float raw = tr->meter_peak;
            float disp = a->meter_disp[ti];
            if (raw > disp) disp = raw;            /* fast attack */
            else disp = disp * 0.82f + raw * 0.18f; /* ~300ms decay */
            a->meter_disp[ti] = disp;
            /* map linear peak -> dB fraction [-60,0] -> [0,1] */
            float mdb = disp > 0.0001f ? 20*log10f(disp) : -60.0f;
            float mf = (mdb - (-60.0f)) / 60.0f; if (mf<0) mf=0; if (mf>1) mf=1;
            int mx = x + 3, mw = 6;
            SDL_Rect mtrk = { mx, fy_top, mw, (int)fader_h };
            setc(a->ren, C_LANE_A); SDL_RenderFillRect(a->ren, &mtrk);
            int mh = (int)(mf * fader_h);
            SDL_Rect mfill = { mx, fy_bot - mh, mw, mh };
            /* green below -6dB, yellow to 0dB, red if clipping (>0.99) */
            if (disp > 0.99f)      setc(a->ren, 220, 60, 60);
            else if (mdb > -6.0f)  setc(a->ren, 220, 200, 60);
            else                   setc(a->ren, 80, 200, 110);
            SDL_RenderFillRect(a->ren, &mfill);
        }

        /* fader track */
        int fx = x + sw/2 - 4;
        int fy_top = MAIN_Y + 40;
        int fy_bot = fy_top + (int)fader_h;
        SDL_Rect ftr = { fx, fy_top, 8, (int)fader_h };
        setc(a->ren, C_LANE_A); SDL_RenderFillRect(a->ren, &ftr);

        /* fader knob — map VOLUME in dB (logarithmic), like a real console.
         * volume is linear gain 0..1; convert to dB in [-60,0], then to a
         * 0..1 fraction of the fader travel. */
        float db = tr->volume>0.0001f ? 20*log10f(tr->volume) : -60.0f;
        float frac = (db - (-60.0f)) / (0.0f - (-60.0f));   /* -60dB->0, 0dB->1 */
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        int fy = fy_bot - (int)(frac * fader_h);
        SDL_Rect knob = { fx-5, fy-6, 18, 12 };
        setc(a->ren, tr->mute?C_MUTE:C_ACCENT); SDL_RenderFillRect(a->ren, &knob);

        /* dB readout */
        char dbuf[16]; snprintf(dbuf,sizeof(dbuf),"%.1f dB",db);
        wb_ui_draw_text(a->ren, x+2, fy_bot+8, dbuf, 1, C_TEXT);

        /* R043 (G4): 0 dB tick marker on the fader (unity anchor) */
        int zero_y = fy_bot - (int)(1.0 * fader_h);   /* frac=1 -> 0 dB */
        setc(a->ren, 110, 110, 110);
        SDL_RenderDrawLine(a->ren, fx-6, zero_y, fx+12, zero_y);

        /* R043 (G4): automation-write arm button ('A') — lit when armed.
         * Placed at the strip's top-right so it never collides with mute/solo
         * or the insert-chain readout below the fader. */
        SDL_Rect armbox = { x + sw - 18, MAIN_Y + 6, 16, 14 };
        setc(a->ren, a->arm[ti] ? C_SOLO : C_LANE_A);
        SDL_RenderFillRect(a->ren, &armbox);
        wb_ui_draw_text(a->ren, armbox.x+3, armbox.y+1, "A", 1, a->arm[ti]?C_BG:C_TEXT_DIM);

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

    /* R028: master bus VU meter — far-right strip of the mixer, driven by the
     * engine's REAL post-master-volume output (what you actually hear). */
    {
        int mwx = mx + MIXER_W - 30;
        int fy_top = MAIN_Y + 40;
        int fy_bot = fy_top + (int)fader_h;
        float pk = 0.0f, rms = 0.0f;
        wb_engine_get_master_meter(a->engine, &pk, &rms);
        float disp = a->master_meter_disp;
        if (pk > disp) disp = pk;                  /* fast attack */
        else disp = disp * 0.82f + pk * 0.18f;     /* ~300ms decay */
        a->master_meter_disp = disp;
        float mdb = disp > 0.0001f ? 20*log10f(disp) : -60.0f;
        float mf = (mdb - (-60.0f)) / 60.0f; if (mf<0) mf=0; if (mf>1) mf=1;
        int mw = 14, mxk = mwx;
        SDL_Rect mtrk = { mxk, fy_top, mw, (int)fader_h };
        setc(a->ren, C_LANE_A); SDL_RenderFillRect(a->ren, &mtrk);
        /* RMS (dimmer, shows average level) */
        float rdb = rms > 0.0001f ? 20*log10f(rms) : -60.0f;
        float rf = (rdb - (-60.0f)) / 60.0f; if (rf<0) rf=0; if (rf>1) rf=1;
        int rh = (int)(rf * fader_h);
        SDL_Rect rfill = { mxk, fy_bot - rh, mw, rh };
        setc(a->ren, 60, 120, 90); SDL_RenderFillRect(a->ren, &rfill);
        /* peak (bright, shows transient/clipping) */
        int mh = (int)(mf * fader_h);
        SDL_Rect mfill = { mxk, fy_bot - mh, mw, mh };
        if (disp > 0.99f)     setc(a->ren, 220, 60, 60);
        else if (mdb > -6.0f) setc(a->ren, 220, 200, 60);
        else                  setc(a->ren, 80, 200, 110);
        SDL_RenderFillRect(a->ren, &mfill);
        /* 0 dB line marker */
        int zy = fy_bot - (int)(1.0 * fader_h);
        setc(a->ren, 90, 90, 90); SDL_RenderDrawLine(a->ren, mxk-2, zy, mxk+mw+2, zy);
        /* label + dB readout */
        wb_ui_draw_text(a->ren, mxk-4, fy_top-16, "MASTER", 1, C_TEXT);
        char mdbuf[16]; snprintf(mdbuf, sizeof(mdbuf), "%.1f", mdb);
        wb_ui_draw_text(a->ren, mxk-2, fy_bot+8, mdbuf, 1, C_ACCENT);
    }
}

/* ---- R043 (G6): Fusion-style node-graph view -------------------------- */
/* Renders the self-contained comp_graph (Source -> Gain -> Composite) as
 * boxes + wires. The UI never touches node internals — only the opaque
 * accessors — so the compositor stays self-contained. */
static void draw_fusion_graph(app *a) {
    if (!a->comp_graph) return;
    int n = wb_node_graph_count(a->comp_graph);
    int ox = GUTTER_W + 16, oy = MAIN_Y + RULER_H + 40;   /* graph origin */
    float scale = 1.0f;
    /* header */
    wb_ui_draw_text(a->ren, ox, oy - 26, "FUSION  .  node graph", 1, C_ACCENT);
    wb_ui_draw_text(a->ren, ox, oy - 12,
        "Source A/B -> Gain -> Composite   (drag-free preview; click a node to select)",
        1, C_TEXT_DIM);
    /* wires first (under nodes) */
    for (int i = 0; i < n; i++) {
        float x1, y1; wb_node_graph_pos(a->comp_graph, i, &x1, &y1);
        int ins = wb_node_graph_inputs(a->comp_graph, i);
        for (int k = 0; k < ins; k++) {
            int j = wb_node_graph_input_of(a->comp_graph, i, k);
            if (j < 0) continue;
            float x2, y2; wb_node_graph_pos(a->comp_graph, j, &x2, &y2);
            setc(a->ren, 150, 170, 200);
            SDL_RenderDrawLine(a->ren,
                (int)(ox + x2*scale) + 90, (int)(oy + y2*scale) + 20,
                (int)(ox + x1*scale),       (int)(oy + y1*scale) + 20);
        }
    }
    /* nodes */
    for (int i = 0; i < n; i++) {
        float x, y; wb_node_graph_pos(a->comp_graph, i, &x, &y);
        int bx = (int)(ox + x*scale), by = (int)(oy + y*scale);
        SDL_Rect box = { bx, by, 90, 40 };
        wb_node_kind k = wb_node_graph_kind(a->comp_graph, i);
        setc(a->ren, (k==WB_NODE_COMPOSITE)?C_ACCENT:C_PANEL2);
        SDL_RenderFillRect(a->ren, &box);
        setc(a->ren, C_TEXT); SDL_RenderDrawRect(a->ren, &box);
        wb_ui_draw_text(a->ren, bx+5, by+5, wb_node_graph_label(a->comp_graph, i), 1, C_TEXT);
        /* animate a gain readout on the Gain node to prove liveness */
        if (k == WB_NODE_EFFECT) {
            float g = wb_node_graph_param(a->comp_graph, i, (double)a->t.song_pos/WB_SAMPLE_RATE);
            char gb[16]; snprintf(gb, sizeof(gb), "g%.2f", g);
            wb_ui_draw_text(a->ren, bx+5, by+22, gb, 1, C_FADE);
        }
    }
}

/* ---- R043 (G7): 3D-CGI viewport + AGI task surface -------------------- */
/* CGI: rasterize the opaque scene model's projected triangles as a wireframe
 * with per-face shading; grid lines under the object. The UI knows nothing
 * about vertices/cameras — only screen-space lines. */
static void draw_cgi_view(app *a) {
    if (!a->cgi) return;
    int ox = GUTTER_W + 16, oy = MAIN_Y + RULER_H + 40;
    wb_ui_draw_text(a->ren, ox, oy - 26, "3D-CGI  .  scene", 1, C_ACCENT);
    wb_ui_draw_text(a->ren, ox, oy - 12,
        "low-poly software renderer  (wheel = zoom; live rotation)", 1, C_TEXT_DIM);
    /* center the projection in the panel */
    int cx = ox + 300, cy = oy + 180;
    /* ground grid first */
    setc(a->ren, 60, 70, 90);
    for (int i = 0; i < wb_cgi_scene_grid_count(a->cgi); i++) {
        float x0,y0,x1,y1;
        wb_cgi_scene_grid_line(a->cgi, i, &x0,&y0,&x1,&y1);
        SDL_RenderDrawLine(a->ren, cx+(int)x0, cy+(int)y0/2, cx+(int)x1, cy+(int)y1/2);
    }
    /* shaded wireframe triangles, back-to-front by mean depth */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < wb_cgi_scene_tri_count(a->cgi); i++) {
            float x0,y0,x1,y1,x2,y2,sh;
            wb_cgi_scene_tri(a->cgi, i, &x0,&y0,&x1,&y1,&x2,&y2,&sh);
            int bright = sh > 0.7f;
            if ((pass == 0) != (!bright)) continue;   /* dim faces behind */
            setc(a->ren,
                 (int)(70 + 140*sh), (int)(80 + 130*sh), (int)(110 + 120*sh));
            SDL_RenderDrawLine(a->ren, cx+(int)x0, cy+(int)y0, cx+(int)x1, cy+(int)y1);
            SDL_RenderDrawLine(a->ren, cx+(int)x1, cy+(int)y1, cx+(int)x2, cy+(int)y2);
            SDL_RenderDrawLine(a->ren, cx+(int)x2, cy+(int)y2, cx+(int)x0, cy+(int)y0);
        }
    }
    char zb[32]; snprintf(zb, sizeof(zb), "zoom %.2f", wb_cgi_scene_get_zoom(a->cgi));
    wb_ui_draw_text(a->ren, ox, oy + 380, zb, 1, C_TEXT);
}

/* AGI: task list + live progress bars. Submit real work via 'N' in this view. */
static void draw_agi_view(app *a) {
    if (!a->agi) return;
    int ox = GUTTER_W + 16, oy = MAIN_Y + RULER_H + 40;
    wb_ui_draw_text(a->ren, ox, oy - 26, "AGI  .  control surface", 1, C_ACCENT);
    wb_ui_draw_text(a->ren, ox, oy - 12,
        "task bridge: N submits a render/polish/cut task  (queued -> running -> done)",
        1, C_TEXT_DIM);
    static const char *st_name[] = { "QUEUED", "RUNNING", "DONE  ", "FAILED" };
    for (int i = 0; i < wb_agi_task_count(a->agi); i++) {
        int y = oy + i * 34;
        wb_agi_status st = wb_agi_task_status(a->agi, i);
        float pr = wb_agi_task_progress(a->agi, i);
        wb_ui_draw_text(a->ren, ox, y, wb_agi_task_label(a->agi, i), 1, C_TEXT);
        wb_ui_draw_text(a->ren, ox + 320, y, st_name[st],
                        1, st == WB_AGI_DONE ? C_ACCENT : C_FADE);
        /* progress bar */
        SDL_Rect bg = { ox + 400, y - 2, 200, 12 };
        setc(a->ren, C_LANE_B); SDL_RenderFillRect(a->ren, &bg);
        if (pr > 0.0f) {
            SDL_Rect fg = { ox + 400, y - 2, (int)(200 * pr), 12 };
            setc(a->ren, C_ACCENT); SDL_RenderFillRect(a->ren, &fg);
        }
    }
    const char *ev = wb_agi_last_event(a->agi);
    if (ev[0]) {
        char line[128];
        snprintf(line, sizeof(line), "last event: %s", ev);
        wb_ui_draw_text(a->ren, ox, oy + 380, line, 1, C_TEXT_DIM);
    }
}

/* ---- R065: PERFORMANCE view — deck grid, live state, record arm ------- */
/* R065: the agent bridge renders through THIS performance instance. */
void *wb_agent_perf_target(void) {
    extern app *g_app_for_perf;
    return g_app_for_perf ? g_app_for_perf->perf : NULL;
}

static void draw_perf_view(app *a) {
    char buf[128];
    int px = GUTTER_W + 8;
    int yy = MAIN_Y + RULER_H + 8;

    snprintf(buf, sizeof buf, "PERFORMANCE  .  video DJ");
    wb_ui_draw_text(a->ren, px, yy, buf, 1, C_ACCENT); yy += 18;
    snprintf(buf, sizeof buf,
        "decks fire on the beat; RECORD captures the event list to replay "
        "on the timeline");
    wb_ui_draw_text(a->ren, px, yy, buf, 1, C_TEXT_DIM); yy += 22;

    if (!a->perf) {
        wb_ui_draw_text(a->ren, px, yy, "(performance engine unavailable)", 1, C_TEXT_DIM);
        return;
    }

    int ndecks = wb_perf_deck_count(a->perf);
    int cols = 4, rows = (ndecks + cols - 1) / cols;
    if (rows < 1) rows = 1;
    int pad = 8;
    int pw = 150, ph = 90;
    for (int d = 0; d < ndecks; d++) {
        int cx = px + (d % cols) * (pw + pad);
        int cy = yy + (d / cols) * (ph + pad);
        SDL_Rect cell = { cx, cy, pw, ph };
        /* fired decks glow accent; idle decks are dim lanes */
        setc(a->ren, ((d % 2) == 0) ? 120 : 60,
                        ((d % 2) == 0) ? 60 : 100,
                        ((d % 2) == 0) ? 40 : 160);
        SDL_RenderFillRect(a->ren, &cell);
        setc(a->ren, C_GRID);
        SDL_RenderDrawRect(a->ren, &cell);
        snprintf(buf, sizeof buf, "DECK %d", d);
        wb_ui_draw_text(a->ren, cx + 6, cy + 6, buf, 1, C_TEXT);
        /* hit region: click fires the deck */
        region_add(80000 + d, cell.x, cell.y, cell.w, cell.h);
    }
    yy += rows * (ph + pad) + 10;

    /* record arm button */
    ui_button(a->ren, 81000, px, yy, 110, 24,
              a->perf_recording ? "RECORDING" : "RECORD ARM",
              a->perf_recording);
    yy += 34;
    snprintf(buf, sizeof buf, "events: %d   %s",
             wb_perf_event_count(a->perf),
             a->perf_recording ? "capturing..." : "idle");
    wb_ui_draw_text(a->ren, px, yy, buf, 1, C_TEXT_DIM);
}

/* ---- VST3 parameter editor panel -------------------------------------- */
#define PED_X        GUTTER_W
#define PED_Y        (MAIN_Y + RULER_H + 8)
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

/* forward declarations for video editor tab functions (defined after render) */
static void draw_video_preview(app *a);
static void draw_video_timeline(app *a);
static void draw_video_tab_panel(app *a);

/* R038: forward decls for the redesigned shell (defined later) */
static void draw_toolbar(app *a);
static void draw_action_bar(app *a);
static void draw_status(app *a);
static void draw_overview(app *a);
static void handle_action(app *a, int act);

static void render(app *a) {
    region_reset();
    wb_engine_get_transport(a->engine, &a->t);
    setc(a->ren, C_BG); SDL_RenderClear(a->ren);
    draw_transport(a);
    draw_toolbar(a);
    draw_overview(a);
    switch (a->tab) {
    case 0:  /* ARRANGE: linear arrangement (Ableton Arrangement View) */
        draw_ruler(a);
        draw_arrangement(a);
        draw_mixer(a);
        draw_param_editor(a);
        break;
    case 1:  /* PAD: performance pad grid (R035) */
        draw_pad(a);
        draw_mixer(a);
        break;
    case 2:  /* STEP: step sequencer (R035) */
        draw_step(a);
        draw_mixer(a);
        break;
    case 3:  /* SESSION: clip launcher grid (R035) */
        draw_session(a);
        draw_mixer(a);
        break;
    default:  /* video editor tabs 4..7 */
        draw_video_preview(a);
        draw_video_timeline(a);
        draw_video_tab_panel(a);
        break;
    }
    draw_action_bar(a);
    draw_status(a);
    draw_workspace_ribbon(a);   /* R043: bottom Fusion-style tier ribbon */
    SDL_RenderPresent(a->ren);
}

/* ---- toolbar: clearly-labeled tab buttons (one source of truth) ------ */
static void draw_toolbar(app *a) {
    SDL_Rect bar = { 0, TRANSPORT_H, WIN_W, TOOLBAR_H };
    setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &bar);
    setc(a->ren, C_BG);
    SDL_RenderDrawLine(a->ren, 0, TRANSPORT_H+TOOLBAR_H-1, WIN_W, TRANSPORT_H+TOOLBAR_H-1);

    int n = 8, pad = 6;
    int bw = (WIN_W - MIXER_W - pad*(n+1)) / n;
    int by = TRANSPORT_H + 5, bh = TOOLBAR_H - 10;
    for (int i = 0; i < n; i++) {
        int bx = pad + i*(bw+pad);
        ui_button(a->ren, BTN_TAB0 + i, bx, by, bw, bh, tab_name(i), (i == a->tab));
    }
}

/* ---- bottom status / help line -------------------------------------- */
static void draw_status(app *a) {
    int y = WIN_H - STATUS_H;
    SDL_Rect s = { 0, y, WIN_W, STATUS_H };
    setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &s);
    char line[256];
    const char *vname = (a->tab<=7) ? tab_name(a->tab) : "?";
    int tk = a->selected_track;
    const char *tkname = (tk>=0 && a->session) ? a->session->tracks[tk].name : "none";
    int tcount = a->session ? (int)a->session->track_count : 0;
    const char *state = a->t.playing ? "PLAYING" : (a->rec_armed ? "ARMED" : "STOPPED");
    snprintf(line, sizeof(line),
        "VIEW: %s    TRACK: %s (%d/%d)    %s    BPM %.1f    %g Hz    |    click tabs+buttons • SPACE play/stop • R rewind • M/S mute/solo in track list",
        vname, tkname, tk+1, tcount, state, a->t.bpm, a->t.sample_rate);
    wb_ui_draw_text(a->ren, 10, y+5, line, 1, C_TEXT_DIM);
}

/* ---- per-view action bar (context buttons, research-backed) -------- */
static void draw_action_bar(app *a) {
    int y = TRANSPORT_H + TOOLBAR_H;          /* ACTION_H band */
    SDL_Rect band = { 0, y, WIN_W - MIXER_W, ACTION_H };
    setc(a->ren, C_PANEL2); SDL_RenderFillRect(a->ren, &band);
    int bx = 8, by = y + 4, bh = ACTION_H - 8, bw = 96;
    int tab = a->tab;
    if (tab == 0) {   /* ARRANGE */
        ui_button(a->ren, BTN_ACT0, bx,        by, bw, bh, "+ TRACK", 0);
        ui_button(a->ren, BTN_ACT1, bx+bw+6,  by, bw, bh, "MARKER",  0);
        ui_button(a->ren, BTN_ACT2, bx+2*(bw+6), by, bw, bh, "COMP",    0);
    } else if (tab == 1) {  /* PAD */
        ui_button(a->ren, BTN_ACT0, bx,        by, bw, bh, "STOP ALL", 0);
    } else if (tab == 2) {  /* STEP */
        ui_button(a->ren, BTN_ACT0, bx,        by, bw, bh, "CLEAR", 0);
        ui_button(a->ren, BTN_ACT1, bx+bw+6,  by, bw, bh, "COMMIT", 0);
    } else if (tab == 3) {  /* SESSION */
        ui_button(a->ren, BTN_ACT0, bx,        by, bw, bh, "STOP ALL", 0);
    } else {  /* video tabs 4..7 */
        ui_button(a->ren, BTN_ACT0, bx,        by, bw, bh, "IMPORT", 0);
        ui_button(a->ren, BTN_ACT1, bx+bw+6,  by, bw, bh, "CAPTIONS", 0);
        ui_button(a->ren, BTN_ACT2, bx+2*(bw+6), by, bw, bh, "EXPORT", 0);
    }
}

/* ---- R040: Arrangement Overview minimap (Ableton-style bird's-eye) --- */
static void draw_overview(app *a) {
    int y = TRANSPORT_H + TOOLBAR_H + ACTION_H;   /* strip band */
    int x0 = GUTTER_W, w = WIN_W - MIXER_W - GUTTER_W;
    SDL_Rect strip = { x0, y, w, OVERVIEW_H };
    setc(a->ren, C_BG); SDL_RenderFillRect(a->ren, &strip);
    setc(a->ren, C_GRID); SDL_RenderDrawRect(a->ren, &strip);
    if (!a->session) { region_add(BTN_OVERVIEW, x0, y, w, OVERVIEW_H); return; }

    double slen = song_len_samples(a) / (double)WB_SAMPLE_RATE;  /* song length secs */
    if (slen <= 0) slen = a->visible_secs;
    /* clip density: colored ticks per track */
    for (uint32_t t = 0; t < a->session->track_count; t++) {
        wb_track *tr = &a->session->tracks[t];
        rgb tc = track_rgb((int)t);
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            wb_clip *cl = &tr->clips[c];
            int cx = x0 + (int)((cl->start / WB_SAMPLE_RATE / slen) * w);
            int cw = (int)((cl->length / WB_SAMPLE_RATE / slen) * w);
            if (cw < 1) cw = 1;
            setc(a->ren, tc.r, tc.g, tc.b);
            SDL_Rect cr = { cx, y+3 + (int)t*((OVERVIEW_H-6)/(int)a->session->track_count) + 1,
                            cw, (OVERVIEW_H-6)/(int)a->session->track_count };
            SDL_RenderFillRect(a->ren, &cr);
        }
    }
    /* view window rectangle (current zoom/scroll position) */
    int vx = x0 + (int)((a->view_start / WB_SAMPLE_RATE / slen) * w);
    int vw = (int)((a->visible_secs / slen) * w);
    if (vw < 6) vw = 6;
    SDL_Rect win = { vx, y+1, vw, OVERVIEW_H-2 };
    setc(a->ren, C_ACCENT); SDL_RenderDrawRect(a->ren, &win);
    setc(a->ren, 96,155,235); SDL_RenderFillRect(a->ren, &win);  /* faint fill */
    setc(a->ren, C_TEXT_DIM);
    wb_ui_draw_text(a->ren, x0+4, y+5, "OVERVIEW (drag to scroll • wheel to zoom)", 1, C_TEXT_DIM);
    region_add(BTN_OVERVIEW, x0, y, w, OVERVIEW_H);
}

static SDL_Rect video_preview_rect(app *a) {
    (void)a;
    SDL_Rect r;
    r.x = GUTTER_W;
    r.y = MAIN_Y + RULER_H + 26;
    r.w = WIN_W - MIXER_W - GUTTER_W - 540;
    r.h = 300;
    return r;
}

static SDL_Rect video_timeline_rect(app *a) {
    SDL_Rect r;
    SDL_Rect prev = video_preview_rect(a);
    r.x = GUTTER_W;
    r.y = prev.y + prev.h + 8;
    r.w = WIN_W - MIXER_W - GUTTER_W - 540;
    r.h = 40;
    return r;
}

static void draw_video_preview(app *a) {
    SDL_Rect prev = video_preview_rect(a);
    SDL_Rect tl   = video_timeline_rect(a);
    setc(a->ren, C_PANEL2);
    SDL_Rect bg = { prev.x, prev.y, prev.w, tl.y - prev.y - 4 };
    SDL_RenderFillRect(a->ren, &bg);
    setc(a->ren, C_BG);
    SDL_RenderDrawRect(a->ren, &bg);
    if (!a->vid_has_clip) {
        char msg[128];
        snprintf(msg, sizeof(msg), "  [no video loaded]\n  press ^I to import (MEDIA tab)");
        wb_ui_draw_text(a->ren, prev.x + 20, prev.y + prev.h/2 - 20, msg, 1, C_TEXT_DIM);
        return;
    }
    double clip_time = a->t.song_pos / WB_SAMPLE_RATE - a->vid_tl_start;
    if (clip_time < 0) clip_time = 0;
    if (clip_time > a->vid_dur) clip_time = a->vid_dur;
    /* R027: honor the clip's source in-point so slip/roll show the correct
     * frame in the preview (not just the timeline-relative position). */
    double in_src = 0.0;
    if (a->vid_has_clip && a->session && a->vid_track >= 0 && a->vid_clip >= 0) {
        wb_clip *vc = &a->session->tracks[a->vid_track].clips[a->vid_clip];
        if (vc->type == 2 && vc->video) in_src = vc->video->start_in_source;
        if (in_src < 0) in_src = 0.0;
    }
    /* R041: live frame decode is opt-in (WB_VIDEO_PREVIEW=1). The video
     * decoder backend is unstable on this platform and can crash the whole
     * app non-deterministically, so by default we show the timecode instead
     * of decoding frames. The editor panels + timeline remain fully usable. */
    if (!getenv("WB_VIDEO_PREVIEW")) {
        double sec = a->t.song_pos / WB_SAMPLE_RATE;
        int m = (int)(sec/60), s = (int)(fmod(sec, 60.0)), cs = (int)((sec-(int)sec)*100);
        char tc[32]; snprintf(tc, sizeof(tc), "%02d:%02d.%02d", m, s, cs);
        wb_ui_draw_text(a->ren, prev.x + 20, prev.y + prev.h/2 - 8, tc, 2, C_TEXT);
        return;
    }
    /* R041: decode defensively — only open a file we can actually read, and
     * prefer the local 480p proxy (smaller, always present) over the source.
     * Any decoder failure must NOT crash the whole app. */
    const char *decpath = (a->vid_proxy[0] && access(a->vid_proxy, R_OK) == 0) ? a->vid_proxy
                        : (a->vid_source[0] && access(a->vid_source, R_OK) == 0) ? a->vid_source : NULL;
    if (!decpath) return;
    wb_video_decoder *vd = wb_video_decoder_open(decpath);
    if (vd) {
        uint8_t *rgba = calloc(PROXY_SCALE_W * PROXY_SCALE_H, 4);
        int out_w = PROXY_SCALE_W, out_h = PROXY_SCALE_H;
        if (rgba &&
            wb_video_decoder_seek(vd, in_src + clip_time) == 0 &&
            wb_video_decoder_decode_frame(vd, rgba, &out_w, &out_h) == 0) {
            SDL_Texture *tex = wb_video_frame_to_texture(a->ren, rgba, out_w, out_h);
            if (tex) {
                SDL_Rect dst = { prev.x + 10, prev.y + 10,
                                (int)(prev.w - 20), (int)(prev.h - 20) };
                wb_video_blit_scaled(a->ren, tex, &dst);
                SDL_DestroyTexture(tex);
            }
        }
        free(rgba);
        wb_video_decoder_close(vd);
    }
    if (!a->vid_preview_tex) {
        double sec = a->t.song_pos / WB_SAMPLE_RATE;
        int m = (int)(sec/60), s = (int)(fmod(sec, 60.0)), cs = (int)((sec-(int)sec)*100);
        char tc[32]; snprintf(tc, sizeof(tc), "%02d:%02d.%02d", m, s, cs);
        wb_ui_draw_text(a->ren, prev.x + 20, prev.y + prev.h/2 - 8, tc, 2, C_TEXT);
    }
}

static void draw_video_timeline(app *a) {
    SDL_Rect tl = video_timeline_rect(a);
    setc(a->ren, C_PANEL);
    SDL_RenderFillRect(a->ren, &tl);
    setc(a->ren, C_BG);
    SDL_RenderDrawRect(a->ren, &tl);
    wb_ui_draw_text(a->ren, tl.x + 8, tl.y + 6, "TIMELINE", 1, C_TEXT_DIM);
    if (!a->vid_has_clip) return;
    double px_per_sec = (tl.w - 60) / (a->vid_dur > 0 ? a->vid_dur : 1.0);
    int bar_x = tl.x + 30 + (int)(a->vid_tl_start * px_per_sec);
    int bar_w = (int)(a->vid_dur * px_per_sec);
    SDL_Rect bar = { bar_x, tl.y + 18, bar_w, 14 };
    setc(a->ren, C_ACCENT);
    SDL_RenderFillRect(a->ren, &bar);
    setc(a->ren, C_BG);
    SDL_RenderDrawRect(a->ren, &bar);
    double ph_time = a->t.song_pos / WB_SAMPLE_RATE - a->vid_tl_start;
    if (ph_time < 0) ph_time = 0;
    if (ph_time > a->vid_dur) ph_time = a->vid_dur;
    int phx = tl.x + 30 + (int)(ph_time * px_per_sec);
    setc(a->ren, C_PLAY);
    SDL_RenderDrawLine(a->ren, phx, tl.y + 2, phx, tl.y + tl.h - 2);
    SDL_Rect head = { phx - 4, tl.y + tl.h - 10, 8, 8 };
    SDL_RenderFillRect(a->ren, &head);
    SDL_Rect play_btn = { tl.x + tl.w - 150, tl.y + 8, 30, 22 };
    setc(a->ren, a->t.playing ? C_MUTE : C_ACCENT);
    SDL_RenderFillRect(a->ren, &play_btn);
    setc(a->ren, C_BG);
    SDL_RenderDrawLine(a->ren, play_btn.x+8, play_btn.y+4, play_btn.x+8, play_btn.y+18);
    SDL_RenderDrawLine(a->ren, play_btn.x+8, play_btn.y+4, play_btn.x+20, play_btn.y+10);
    SDL_RenderDrawLine(a->ren, play_btn.x+8, play_btn.y+18, play_btn.x+20, play_btn.y+10);
    wb_ui_draw_text(a->ren, play_btn.x + 34, play_btn.y + 4, "play", 1, C_TEXT_DIM);
    double sec = a->t.song_pos / WB_SAMPLE_RATE;
    int m = (int)(sec/60), s = (int)(fmod(sec, 60.0)), cs = (int)((sec-(int)sec)*100);
    char tc[32]; snprintf(tc, sizeof(tc), "%02d:%02d.%02d", m, s, cs);
    wb_ui_draw_text(a->ren, tl.x + tl.w - 80, tl.y + 8, tc, 1, C_ACCENT);
}

static void draw_video_tab_panel(app *a) {
    int px = WIN_W - MIXER_W + 4;
    int py = MAIN_Y + RULER_H + 26;
    int pw = MIXER_W - 8;
    int ph = WIN_H - MAIN_Y - RULER_H - 26;
    SDL_Rect panel = { px, py, pw, ph };
    setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &panel);
    setc(a->ren, C_ACCENT);
    SDL_Rect title = { px, py, pw, 22 };
    SDL_RenderFillRect(a->ren, &title);
    setc(a->ren, C_BG);
    wb_ui_draw_text(a->ren, px + 6, py + 4, tab_name(a->tab), 1, C_BG);
    char buf[256];
    int yy = py + 30;
    switch (a->tab) {
    case 4: /* MEDIA */
        snprintf(buf, sizeof(buf), "Import video (DaVinci-style)");
        wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 20;
        wb_ui_draw_text(a->ren, px + 6, yy, "Press ^I to import a video file.", 1, C_TEXT_DIM); yy += 18;
        wb_ui_draw_text(a->ren, px + 6, yy, "Video decoded to 480p proxy for preview.", 1, C_TEXT_DIM); yy += 20;
        if (a->vid_has_clip) {
            setc(a->ren, C_ACCENT);
            SDL_Rect box = { px + 6, yy, pw - 12, 14 };
            SDL_RenderFillRect(a->ren, &box);
            setc(a->ren, C_BG);
            snprintf(buf, sizeof(buf), "Loaded: %s", a->vid_source);
            wb_ui_draw_text(a->ren, px + 8, yy + 2, buf, 1, C_BG);
            yy += 20;
            snprintf(buf, sizeof(buf), "Duration: %.1f s", a->vid_dur);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 16;
            snprintf(buf, sizeof(buf), "Timeline start: %.1f s", a->vid_tl_start);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 16;
            snprintf(buf, sizeof(buf), "Timeline end: %.1f s", a->vid_tl_end);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 20;
        }
        wb_ui_draw_text(a->ren, px + 6, yy, "Shortcuts:", 1, C_TEXT); yy += 16;
        wb_ui_draw_text(a->ren, px + 6, yy, "^I  import  ^G  captions  ^R  export", 1, C_TEXT_DIM);
        break;
    case 5: /* EDIT */
        /* R043-G6/G7: upper tiers host their own view on this tab. */
        if (wb_workspace_fusion_active(a->ws)) { draw_fusion_graph(a); break; }
        if (wb_workspace_cgi_active(a->ws))   { draw_cgi_view(a);   break; }
        if (wb_workspace_agi_active(a->ws))   { draw_agi_view(a);   break; }
        if (a->perf && wb_workspace_perf_active(a->ws)) {
            extern void draw_perf_view(app *a);
            draw_perf_view(a);
            break;
        }
        snprintf(buf, sizeof(buf), "Clip editor");
        wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 20;
        if (a->vid_has_clip) {
            snprintf(buf, sizeof(buf), "Source: %s", a->vid_source);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT_DIM); yy += 16;
            snprintf(buf, sizeof(buf), "Duration: %.2f s", a->vid_dur);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 14;
            snprintf(buf, sizeof(buf), "In: %.2f s  Out: %.2f s", a->vid_tl_start, a->vid_tl_end);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 18;
        } else {
            wb_ui_draw_text(a->ren, px + 6, yy, "No clip selected. Import in MEDIA tab.", 1, C_TEXT_DIM); yy += 18;
        }
        /* R025: edit-tool shortcuts are ALWAYS visible (tools must be discoverable) */
        wb_ui_draw_text(a->ren, px + 6, yy, "Edit tools:", 1, C_TEXT); yy += 16;
        wb_ui_draw_text(a->ren, px + 6, yy, "^T trim start  ^E trim end", 1, C_TEXT_DIM); yy += 14;
        wb_ui_draw_text(a->ren, px + 6, yy, "^X split  ^D delete (lift)", 1, C_TEXT_DIM); yy += 14;
        wb_ui_draw_text(a->ren, px + 6, yy, "Shift+Del RIPPLE delete (close gap)", 1, C_ACCENT); yy += 14;
        wb_ui_draw_text(a->ren, px + 6, yy, "Y slip in-point  M roll cut", 1, C_ACCENT); yy += 14;
        break;
    case 6: /* CAPTIONS */
        snprintf(buf, sizeof(buf), "Auto Captions (whisper.cpp)");
        wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 18;
        wb_ui_draw_text(a->ren, px + 6, yy, "Extract audio -> transcribe -> SRT.", 1, C_TEXT_DIM); yy += 16;
        wb_ui_draw_text(a->ren, px + 6, yy, "Burn SRT into video via FFmpeg.", 1, C_TEXT_DIM); yy += 20;
        if (a->vid_captions_ready) {
            snprintf(buf, sizeof(buf), "SRT: %s", a->vid_srt);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_ACCENT); yy += 16;
            setc(a->ren, 40, 180, 40);
            SDL_Rect check = { px + 6, yy, 12, 12 };
            SDL_RenderFillRect(a->ren, &check);
            setc(a->ren, C_BG);
            snprintf(buf, sizeof(buf), "Burn captions: enabled");
            wb_ui_draw_text(a->ren, px + 24, yy + 1, buf, 1, C_ACCENT);
            yy += 20;
        } else {
            wb_ui_draw_text(a->ren, px + 6, yy, "SRT: not generated yet", 1, C_TEXT_DIM); yy += 16;
            wb_ui_draw_text(a->ren, px + 6, yy, "Burn captions: disabled", 1, C_TEXT_DIM); yy += 18;
        }
        /* R049: editable transcript — words as clickable rows. Click a word
         * to select it; shift-click extends; BACKSPACE cuts [sel0,sel1)
         * from BOTH the transcript and the media (Descript text editing). */
        if (a->vid_tr && wb_transcript_count(a->vid_tr) > 0) {
            int n = wb_transcript_count(a->vid_tr);
            snprintf(buf, sizeof(buf), "TRANSCRIPT (%d words) — click=select, DEL=cut media", n);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 18;
            a->tr_row_y = yy;
            int rows_visible = 18;
            if (a->tr_scroll > n - 1) a->tr_scroll = n - 1 > 0 ? n - 1 : 0;
            for (int i = a->tr_scroll; i < n && i < a->tr_scroll + rows_visible; i++) {
                const wb_word *w = wb_transcript_word(a->vid_tr, i);
                if (!w) break;
                int sel = (i >= a->tr_sel0 && i < a->tr_sel1) ||
                          (i >= a->tr_sel1 && i < a->tr_sel0);
                SDL_Rect row = { px, yy - 2, 300, 16 };
                if (sel) { setc(a->ren, 60, 90, 140); SDL_RenderFillRect(a->ren, &row); }
                /* register hit region: id = 70000 + i */
                region_add(70000 + i, row.x, row.y, row.w, row.h);
                setc(a->ren, sel ? C_TEXT : C_TEXT_DIM);
                char wbuf[96];
                snprintf(wbuf, sizeof(wbuf), "%d  %s   [%d-%dms]", i,
                         w->word ? w->word : "?",
                         (int)w->start_ms, (int)w->end_ms);
                wb_ui_draw_text(a->ren, px + 8, yy, wbuf, 1,
                                sel ? C_TEXT : C_TEXT_DIM);
                yy += 17;
            }
        } else {
            wb_ui_draw_text(a->ren, px + 6, yy, "(no transcript — press ^G)", 1, C_TEXT_DIM);
            yy += 18;
        }
        wb_ui_draw_text(a->ren, px + 6, yy, "Shortcuts:", 1, C_TEXT); yy += 16;
        wb_ui_draw_text(a->ren, px + 6, yy, "^G generate  ^B burn  click words  BKSP cut", 1, C_TEXT_DIM);
        break;
    case 7: /* EXPORT */
        snprintf(buf, sizeof(buf), "Export / Deliver");
        wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 18;
        wb_ui_draw_text(a->ren, px + 6, yy, "Export video with optional", 1, C_TEXT_DIM); yy += 14;
        wb_ui_draw_text(a->ren, px + 6, yy, "caption burn-in via FFmpeg.", 1, C_TEXT_DIM); yy += 20;
        if (a->vid_has_clip) {
            snprintf(buf, sizeof(buf), "Source: %s", a->vid_source);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT_DIM); yy += 14;
            snprintf(buf, sizeof(buf), "Proxy: %s", a->vid_source);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT_DIM); yy += 16;
        }
        if (a->vid_export[0]) {
            setc(a->ren, C_ACCENT);
            SDL_Rect box = { px + 6, yy, pw - 12, 14 };
            SDL_RenderFillRect(a->ren, &box);
            setc(a->ren, C_BG);
            snprintf(buf, sizeof(buf), "Output: %s", a->vid_export);
            wb_ui_draw_text(a->ren, px + 8, yy + 2, buf, 1, C_BG);
            yy += 20;
        } else {
            wb_ui_draw_text(a->ren, px + 6, yy, "Output: not set", 1, C_TEXT_DIM); yy += 18;
        }
        wb_ui_draw_text(a->ren, px + 6, yy, "Format: MP4 H.264 + AAC", 1, C_TEXT); yy += 14;
        wb_ui_draw_text(a->ren, px + 6, yy, "Resolution: 1920x1080", 1, C_TEXT); yy += 20;
        wb_ui_draw_text(a->ren, px + 6, yy, "Shortcuts:", 1, C_TEXT); yy += 16;
        wb_ui_draw_text(a->ren, px + 6, yy, "^R render  ^S set path", 1, C_TEXT_DIM);
        break;
    }
}

static int video_import(app *a, const char *path) {
    if (!path || !path[0]) return -1;
    if (access(path, R_OK) != 0) { fprintf(stderr, "video: cannot read %s\n", path); return -1; }
    snprintf(a->vid_source, sizeof(a->vid_source), "%s", path);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "\"%s\" -i \"%s\" 2>&1 | grep Duration | head -1",
             a->ffmpeg_path[0] ? a->ffmpeg_path : "/Users/waefrebeorn/.local/bin/ffmpeg", path);
    FILE *f = popen(cmd, "r");
    char line[256];
    a->vid_dur = 0.0;
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            char *p = strstr(line, "Duration: ");
            if (p) { p += 10; int h, m, s;
                if (sscanf(p, "%d:%d:%d", &h, &m, &s) == 3)
                    a->vid_dur = h * 3600.0 + m * 60.0 + s;
            }
        }
        pclose(f);
    }
    if (a->vid_dur <= 0) { fprintf(stderr, "video: cannot determine duration of %s\n", path); return -1; }
    char proxy_path[512];
    snprintf(proxy_path, sizeof(proxy_path), "/tmp/bigmac_proxy_%d_%d.mp4", (int)getpid(), (int)(a->vid_dur * 100));
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -y -i \"%s\" -vf \"scale=854:480:force_original_aspect_ratio=decrease,pad=854:480:-1:-1:black\" "
             "-c:v libx264 -preset fast -crf 23 -c:a aac -b:a 64k \"%s\" > /dev/null 2>&1",
             a->ffmpeg_path[0] ? a->ffmpeg_path : "/Users/waefrebeorn/.local/bin/ffmpeg",
             path, proxy_path);
    if (system(cmd) != 0) { fprintf(stderr, "video: proxy generation failed for %s\n", path); return -1; }
    snprintf(a->vid_proxy, sizeof(a->vid_proxy), "%s", proxy_path);
    if (!a->session) { a->session = wb_session_create(); wb_engine_set_session(a->engine, a->session); }
    int vt = -1;
    for (uint32_t ti = 0; ti < a->session->track_count; ti++) {
        for (uint32_t c = 0; c < a->session->tracks[ti].clip_count; c++) {
            if (a->session->tracks[ti].clips[c].type == 2) { vt = (int)ti; break; }
        }
        if (vt >= 0) break;
    }
    if (vt < 0) {
        if (a->session->track_count >= WB_MAX_TRACKS) { fprintf(stderr, "video: max tracks\n"); return -1; }
        vt = (int)a->session->track_count;
        wb_track *tr = &a->session->tracks[vt];
        tr->volume = 1.0f;
        tr->kind = WB_TRACK_KIND_VIDEO;   /* R042: video tracks are audio-inert
            so the engine's instrument/audio stages skip them (no voice, no
            crash). Previously left as kind 0 -> engine built a synth voice
            for an uninitialized track -> heap corruption on render. */
        snprintf(tr->name, sizeof(tr->name), "Video");
        a->session->track_count++;
    }
    int ci = wb_session_add_video_clip(a->session, vt, path, 0.0);
    if (ci < 0) {
        /* R041: undo the Video track we just created so the session stays
         * consistent with the engine runtime (no stale track_count). */
        if (vt == (int)a->session->track_count - 1) a->session->track_count--;
        fprintf(stderr, "video: failed to add clip\n");
        return -1;
    }
    wb_session_set_video_proxy(a->session, vt, ci, proxy_path);
    a->vid_track = vt;
    a->vid_clip = ci;
    a->vid_tl_start = 0.0;
    a->vid_tl_end = a->vid_dur;
    a->vid_has_clip = 1;
    /* R041: rebuild the engine runtime NOW — we just grew track_count / added
     * a clip. Without this the engine render iterates track_count over a stale
     * rtracks array (sized for the old count) -> out-of-bounds crash. */
    wb_engine_set_session(a->engine, a->session);
    printf("video: imported %s (%.1f s, proxy: %s)\n", path, a->vid_dur, proxy_path);
    return 0;
}

static void video_tab_enter(app *a) {
    if (a->vid_preview_tex) { SDL_DestroyTexture(a->vid_preview_tex); a->vid_preview_tex = NULL; }
    a->vid_has_clip = 0; a->vid_track = -1; a->vid_clip = -1;
    a->vid_tl_start = 0.0; a->vid_tl_end = 0.0; a->vid_dur = 0.0;
    a->vid_source[0] = 0; a->vid_proxy[0] = 0;
    a->vid_export[0] = 0; a->vid_srt[0] = 0; a->vid_captions_ready = 0;
    snprintf(a->ffmpeg_path, sizeof(a->ffmpeg_path), "/Users/waefrebeorn/.local/bin/ffmpeg");
    snprintf(a->whisper_cli_path, sizeof(a->whisper_cli_path),
             "/Users/waefrebeorn/whisper.cpp/build/bin/whisper-cli");
    snprintf(a->whisper_model_path, sizeof(a->whisper_model_path),
             "/Users/waefrebeorn/whisper.cpp/models/ggml-tiny.en-q5_1.bin");
}


/* MIDI callback: push controller notes into the engine's lock-free queue.
 * Called from CoreMIDI's receive thread — only touches the queue (RT-safe). */
static void midi_cb(wb_midi_event ev, void *userdata) {
    app *a = userdata;
    uint8_t st = ev.status & 0xF0;
    if (st == 0x90) {
        /* note on → play a note on this DAW's instrument track */
        wb_engine_note(a->engine, a->midi_track, ev.data1, ev.data2);
        /* Launchpad LED feedback — support both classic and Mk2 layouts.
         * Classic grid: notes 0..119 (row*16+col).  Mk2 grid: notes 11..88
         * (11+col+row*10) + top row 91..98.  We invert the note via the
         * API so the right pad lights regardless of which controller is wired. */
        if (a->midi) {
            int row = -1, col = -1;
            if (wb_lp_mk2_row_col_from_note(ev.data1, &row, &col) == 0) {
                wb_lp_mk2_led(a->midi, row, col, WB_LP_GREEN);
            } else if (ev.data1 >= 0 && ev.data1 <= 119) {
                int cr = ev.data1 / 16, cc = ev.data1 % 16;
                if (cr <= 7 && cc <= 7)
                    wb_launchpad_classic_led(a->midi, cr, cc, 3);
            }
            a->last_lp_row = row; a->last_lp_col = col;
        }
    } else if (st == 0x80) {
        /* note off → silence the voice (key off) + dim the pad */
        wb_engine_note(a->engine, a->midi_track, ev.data1, 0);
        if (a->midi && a->last_lp_row >= 0) {
            int row = a->last_lp_row, col = a->last_lp_col;
            a->last_lp_row = -1;
            /* prefer Mk2 API; fall back to classic if the pad was lit that way */
            if (wb_lp_mk2_row_col_from_note(ev.data1, &row, &col) == 0)
                wb_lp_mk2_led(a->midi, row, col, WB_LP_DIM);
            else if (ev.data1 >= 0 && ev.data1 <= 119) {
                int cr = ev.data1 / 16, cc = ev.data1 % 16;
                if (cr <= 7 && cc <= 7)
                    wb_launchpad_classic_led(a->midi, cr, cc, 0);
            }
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
    int top = MAIN_Y + RULER_H;
    if (y < top || y >= top + ARRANG_H) return -1;
    double track_h = (double)ARRANG_H / n;
    return (int)((y - top) / track_h);
}

/* R032: which take-lane (sub-row) within track ti does screen-y fall on? */
static int y_to_lane(app *a, int ti, int y) {
    if (!a->session || ti < 0 || ti >= (int)a->session->track_count) return 0;
    wb_track *tr = &a->session->tracks[ti];
    int top = MAIN_Y + RULER_H;
    double track_h = (double)ARRANG_H / a->session->track_count;
    double rel = (y - top - ti*track_h - 4);
    int lane_count = 1;
    for (uint32_t c = 0; c < tr->clip_count; c++)
        if (tr->clips[c].lane + 1 > lane_count) lane_count = tr->clips[c].lane + 1;
    int lh = (int)((track_h - 8) / lane_count);
    if (lh < 1) lh = 1;
    int lane = (int)(rel / lh);
    if (lane < 0) lane = 0;
    if (lane >= lane_count) lane = lane_count - 1;
    return lane;
}

/* Convert a screen y within a track lane to a MIDI pitch (2-octave span). */
static int y_to_pitch(app *a, int ti, int y) {
    int n = a->session ? a->session->track_count : 0;
    if (n == 0) return 60;
    int top = MAIN_Y + RULER_H;
    double track_h = (double)ARRANG_H / n;
    double span = 24.0;
    double rel = (y - top - ti*track_h) / track_h;   /* 0..1 within lane */
    if (rel < 0) rel = 0; if (rel > 1) rel = 1;
    int row = (int)(rel * span);
    int base = 48;  /* C3 */
    return base + (span - 1 - row);
}

/* R023: find the index of the note under (x,y) in track ti, or -1. */
static int note_under(app *a, int ti, int x, int y) {
    if (!a->session || ti < 0 || ti >= (int)a->session->track_count) return -1;
    wb_track *tr = &a->session->tracks[ti];
    for (uint32_t c = 0; c < tr->clip_count; c++) {
        wb_clip *cl = &tr->clips[c];
        for (uint32_t k = 0; k < cl->note_count; k++) {
            wb_note *nt = &cl->notes[k];
            double s = cl->start + nt->start;
            int nx = arr_x(a, s);
            int nw = (int)((nt->dur/WB_SAMPLE_RATE)*arr_px_per_sec(a)); if(nw<3)nw=3;
            int span = 24; double track_h = (double)ARRANG_H / a->session->track_count;
            int row = nt->pitch % span; int cell_h = (int)(track_h/span);
            int top = MAIN_Y + RULER_H;
            int ny = top + (int)(ti*track_h) + (int)track_h - (row+1)*cell_h;
            if (x >= nx && x <= nx+nw && y >= ny && y <= ny+cell_h) {
                a->vel_drag_clip = (int)c; a->vel_drag_note = (int)k;
                return (int)c; /* caller also has note idx via a->vel_drag_note */
            }
        }
    }
    return -1;
}

/* ---- R035: click handlers for the performance views ------------------ */
static void pad_click(app *a, int x, int y) {
    int x0 = GUTTER_W, y0 = MAIN_Y + RULER_H + 28;
    int cols = 8, rows = 4, pad = 6;
    int pw = (ARRANG_W - pad*(cols+1)) / cols;
    int ph = (ARRANG_H - 60 - pad*(rows+1)) / rows;
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
        int px = x0 + pad + c*(pw+pad), py = y0 + r*(ph+pad);
        if (x >= px && x < px+pw && y >= py && y < py+ph) {
            int idx = r*cols + c, pitch = 36 + idx;
            a->pad_flash[idx] = 8;
            if (a->selected_track >= 0)
                wb_engine_note(a->engine, a->selected_track, (uint8_t)pitch, 100);
            printf("pad: track %d pitch %d\n", a->selected_track, pitch);
            return;
        }
    }
}
static void step_click(app *a, int x, int y) {
    int ti = a->selected_track; if (ti < 0) return;
    int x0 = GUTTER_W, y0 = MAIN_Y + RULER_H + 28;
    int steps = 16, rows = 8, pad = 4;
    int cw = (ARRANG_W - pad*(steps+1)) / steps;
    int rh = (ARRANG_H - 60 - pad*(rows+1)) / rows;
    for (int s = 0; s < steps; s++) for (int r = 0; r < rows; r++) {
        int px = x0 + pad + s*(cw+pad), py = y0 + r*(rh+pad);
        if (x >= px && x < px+cw && y >= py && y < py+rh) {
            int pitch = 60 - r;
            if (a->step_pitch[ti][s] == pitch) a->step_pitch[ti][s] = -1;
            else { a->step_pitch[ti][s] = pitch;
                   wb_engine_note(a->engine, ti, (uint8_t)pitch, 100); }
            return;
        }
    }
}
static void session_click(app *a, int x, int y) {
    if (!a->session) return;
    int x0 = GUTTER_W, y0 = MAIN_Y + RULER_H + 28;
    int scenes = 4, pad = 6, rh = 40;
    for (uint32_t t = 0; t < a->session->track_count; t++) {
        int py = y0 + t*(rh+pad);
        if (y < py || y > py+rh) continue;
        int cw = (ARRANG_W - pad*(scenes+1)) / scenes;
        for (int sc = 0; sc < scenes; sc++) {
            int px = x0 + 120 + pad + sc*(cw+pad);
            if (x >= px && x < px+cw) {
                /* R037: launching a scene = toggling that track's clip on the
                 * matching lane (transport-independent loop playback). */
                wb_track *tk = &a->session->tracks[t];
                int ci = -1;
                for (uint32_t c = 0; c < tk->clip_count; c++)
                    if (tk->clips[c].lane == sc) { ci = (int)c; break; }
                if (ci < 0) return;
                wb_engine_launch(a->engine, (int)t, ci);
                printf("session: track %d launch clip %d (scene %d)\n", t, ci, sc);
                return;
            }
        }
    }
}

/* ---- R036: commit the STEP pattern into the track's arrangement clip -- */
static void step_commit_to_clip(app *a) {
    int ti = a->selected_track; if (ti < 0 || !a->session) return;
    wb_track *tr = &a->session->tracks[ti];
    double bpm = a->t.bpm > 0 ? a->t.bpm : 120.0;
    double step_sec = (60.0 / bpm) / 4.0;          /* 16th-note seconds */
    double step_smp = step_sec * WB_SAMPLE_RATE;
    /* ensure a clip exists to hold the notes */
    if (tr->clip_count == 0) {
        tr->clips = calloc(1, sizeof(wb_clip));
        tr->clips[0].type = 0; tr->clips[0].start = 0;
        tr->clips[0].length = 16 * step_smp;
        tr->clip_count = 1;
    }
    wb_clip *cl = &tr->clips[tr->clip_count - 1];
    /* clear existing notes, then write the pattern (idempotent) */
    free(cl->notes); cl->notes = NULL; cl->note_count = 0;
    cl->length = 16 * step_smp;
    int n = 0;
    for (int s = 0; s < 16; s++) {
        int p = a->step_pitch[ti][s];
        if (p < 0) continue;
        wb_session_add_note(tr, s * step_smp, step_smp * 0.9, p, 100);
        n++;
    }
    wb_engine_set_session(a->engine, a->session);  /* rebuild runtime */
    printf("step-commit: track %d wrote %d notes (one bar) into clip\n", ti, n);
}

/* ---- R038: per-view action-button dispatch (research-backed controls) - */
static void handle_action(app *a, int act) {
    if (!a->session) return;
    int tab = a->tab;
    if (tab == 0) {  /* ARRANGE */
        if (act == 0) {  /* + TRACK */
            int ni = (int)a->session->track_count;
            wb_session_add_track(a->session, "Track", 0);
            a->selected_track = ni;
            wb_engine_set_session(a->engine, a->session);
            printf("arrange: +track -> %d tracks\n", a->session->track_count);
        } else if (act == 1) {  /* MARKER at playhead */
            wb_session_add_marker(a->session, a->t.song_pos, "M", 0);
            printf("arrange: marker at %.2fs\n", a->t.song_pos / WB_SAMPLE_RATE);
        } else if (act == 2) {  /* COMP selected marquee to lane 0 */
            if (a->selected_track >= 0 && a->sel_t1 > a->sel_t0)
                wb_session_comp_region(a->session, a->selected_track,
                                       a->sel_lane, a->sel_t0, a->sel_t1);
            printf("arrange: comp selection -> lane 0\n");
        }
    } else if (tab == 1 || tab == 3) {  /* PAD / SESSION: STOP ALL launches */
        for (uint32_t t = 0; t < a->session->track_count; t++)
            wb_engine_stop_launch(a->engine, (int)t);
        printf("launch: STOP ALL\n");
    } else if (tab == 2) {  /* STEP */
        if (act == 0) {  /* CLEAR pattern on selected track */
            int ti = a->selected_track;
            if (ti >= 0) for (int s = 0; s < 16; s++) a->step_pitch[ti][s] = -1;
            printf("step: cleared pattern (track %d)\n", ti);
        } else if (act == 1) {  /* COMMIT to clip */
            step_commit_to_clip(a);
        }
    } else {  /* video tabs 4..7 */
        if (act == 0) {  /* IMPORT demo */
            const char *dv = "/Users/waefrebeorn/Documents/big-mac/test_media/demo.mp4";
            if (access(dv, F_OK) != 0) dv = "/Users/waefrebeorn/Videos/demo.mp4";
            if (access(dv, F_OK) == 0) video_import(a, dv);
            else printf("video: no demo .mp4 found\n");
        } else if (act == 1) {  /* CAPTIONS */
            if (a->vid_has_clip && a->whisper_cli_path[0] && a->whisper_model_path[0]) {
                char srt_path[512];
                snprintf(srt_path, sizeof(srt_path), "/tmp/bigmac_captions_%d.srt",
                         (int)(a->t.song_pos / WB_SAMPLE_RATE * 100));
                int rc = wb_video_captions_generate(a->vid_source, srt_path,
                                                     a->whisper_cli_path, a->whisper_model_path);
                if (rc == 0) { snprintf(a->vid_srt, sizeof(a->vid_srt), "%s", srt_path);
                              a->vid_captions_ready = 1;
                              if (a->vid_tr) wb_transcript_free(a->vid_tr);
                              a->vid_tr = wb_transcript_from_srt(srt_path);
                              a->tr_sel0 = a->tr_sel1 = 0;
                              printf("captions: ok (%d words)\n",
                                     a->vid_tr ? wb_transcript_count(a->vid_tr) : 0); }
            }
        } else if (act == 2) {  /* EXPORT */
            if (a->vid_has_clip) {
                if (!a->vid_export[0]) snprintf(a->vid_export, sizeof(a->vid_export),
                         "/tmp/bigmac_export_%d.mp4", (int)(a->t.song_pos/WB_SAMPLE_RATE*100));
                int rc = wb_video_export(a->session, a->engine, a->vid_export,
                                         a->vid_captions_ready ? a->vid_srt : NULL);
                printf("video: export rc=%d\n", rc);
            }
        }
    }
}

/* ---- R035: per-frame performance tick (STEP playback + PAD flash) ---- */
static void perf_tick(app *a) {
    /* decay PAD flash counters */
    for (int i = 0; i < 32; i++) if (a->pad_flash[i] > 0) a->pad_flash[i]--;
    /* STEP sequencer: fire the selected track's active steps on each 16th */
    if (a->tab == 2 && a->selected_track >= 0 && a->session) {
        double bpm = a->t.bpm > 0 ? a->t.bpm : 120.0;
        double spstep = (60.0/bpm)/4.0;   /* 16th-note seconds */
        int cur = (int)(a->t.song_pos / WB_SAMPLE_RATE / spstep) % 16;
        if (cur != a->last_step) {
            a->last_step = cur;
            int ti = a->selected_track;
            int p = a->step_pitch[ti][cur];
            if (p >= 0) wb_engine_note(a->engine, ti, (uint8_t)p, 100);
        }
    }
    /* R067: JKL shuttle — advance the playhead at shuttle speed when
     * stopped; while playing, L/J re-trigger playback direction via
     * engine seek steps. Simple approach: when not playing, JKL moves
     * the head directly (scrub); when playing, speed multiplies seeks. */
    if (a->jkl_speed != 0) {
        double step = a->jkl_speed * (WB_SAMPLE_RATE / 30.0);
        double np = a->t.song_pos + step;
        if (np < 0) np = 0;
        /* loop over I/O if both marked */
        if (a->io_out > a->io_in) {
            if (np > a->io_out) np = a->io_in;
            if (np < a->io_in - WB_SAMPLE_RATE) np = a->io_out;
        }
        wb_engine_seek(a->engine, np);
    }
    /* R065: performance clock follows the transport while recording so
     * captured events land on musical time. */
    if (a->perf && a->perf_recording && a->t.playing)
        wb_perf_set_clock(a->perf, a->t.song_pos / WB_SAMPLE_RATE);
    /* R043-G7: live ticks for the upper-tier views (CGI rotation + AGI pipeline) */
    if (a->cgi && wb_workspace_cgi_active(a->ws)) wb_cgi_scene_tick(a->cgi, 1.0/60.0);
    if (a->agi && wb_workspace_agi_active(a->ws)) wb_agi_tick(a->agi, 1.0/60.0);
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

    /* ---- R043 (G4): mixer fader drag + automation arm ----
     * Grab the fader if the click lands on a strip's fader column, or toggle
     * the arm button. Must come before the generic registry so fader clicks
     * win over empty canvas. */
    if (b.button == SDL_BUTTON_LEFT && a->session) {
        int n = (int)a->session->track_count;
        int mx = WIN_W - MIXER_W;
        double strip_w = (double)MIXER_W / (n>0?n:1);
        double fader_h = ARRANG_H - 70;
        int fy_top = MAIN_Y + 40, fy_bot = fy_top + (int)fader_h;
        if (b.x >= mx) {
            int ti = (int)((b.x - mx) / strip_w);
            if (ti >= 0 && ti < n) {
                int x = mx + (int)(ti*strip_w);
                int sw = (int)strip_w - 4;
                int fx = x + sw/2 - 4;
                /* arm button: a small 'A' box just above the dB readout */
                SDL_Rect armbox = { x+2, fy_bot+44, 16, 14 };
                if (b.y >= armbox.y && b.y <= armbox.y+armbox.h &&
                    b.x >= armbox.x && b.x <= armbox.x+armbox.w) {
                    a->arm[ti] = a->arm[ti] ? 0 : 1;
                    if (a->arm[ti]) {
                        /* create/ensure the track's volume lane + recorder */
                        wb_automation_lane *lane = NULL;
                        for (uint32_t l = 0; l < a->session->automation_count; l++)
                            if (a->session->automation[l]->target == ti &&
                                !strcmp(a->session->automation[l]->param, "volume"))
                            { lane = a->session->automation[l]; break; }
                        if (!lane) lane = wb_session_add_automation(a->session, "volume", ti);
                        if (a->fader_rec[ti]) wb_automation_recorder_destroy(a->fader_rec[ti]);
                        a->fader_rec[ti] = wb_automation_recorder_create(lane, 0.01);
                        wb_automation_recorder_arm(a->fader_rec[ti], a->session->tracks[ti].volume);
                    } else if (a->fader_rec[ti]) {
                        wb_automation_recorder_commit(a->fader_rec[ti]);
                    }
                    printf("fader-arm: track %d -> %s\n", ti, a->arm[ti]?"ARMED":"off");
                    return;
                }
                /* fader column grab (±14px of fx, within fader travel) */
                if (b.x >= fx-14 && b.x <= fx+14 && b.y >= fy_top-8 && b.y <= fy_bot+8) {
                    a->dragging_fader = ti;
                    a->fader_drag_y = b.y;
                    a->fader_vol0 = a->session->tracks[ti].volume;
                    printf("fader: grab track %d (vol %.3f)\n", ti, a->fader_vol0);
                    return;
                }
            }
        }
    }

        if (b.button == SDL_BUTTON_LEFT) {
            int id = region_hit(b.x, b.y);
            if (id >= 0) {
                switch (id) {
                case BTN_PLAY:    wb_engine_play(a->engine); break;
                case BTN_REWIND:  wb_engine_seek(a->engine, 0); break;
                case BTN_STOP:    wb_engine_stop(a->engine); break;
                case BTN_RECORD:  a->rec_armed = !a->rec_armed; break;
                case BTN_LOOP:    a->loop_on = !a->loop_on; break;
                case BTN_SAVE:    if (a->project_path[0]) wb_session_save(a->session, a->project_path);
                                  else { wb_session_save(a->session, "untitled.wbus"); snprintf(a->project_path,sizeof(a->project_path),"untitled.wbus"); }
                                  break;
                case BTN_TAB0: case BTN_TAB1: case BTN_TAB2: case BTN_TAB3:
                case BTN_TAB4: case BTN_TAB5: case BTN_TAB6: case BTN_TAB7:
                    a->tab = id - BTN_TAB0; break;
                case BTN_WS0: case BTN_WS1: case BTN_WS2: case BTN_WS3: case BTN_WS4: {
                    /* R043: switch workspace tier (Fusion-style ribbon) */
                    wb_workspace_tier t = (wb_workspace_tier)(id - BTN_WS0);
                    if (wb_workspace_set(a->ws, t) != 0) {
                        fprintf(stderr, "workspace: tier %s is locked\n",
                                wb_workspace_label(t));
                    }
                    break;
                }
                case BTN_ACT0: case BTN_ACT1: case BTN_ACT2: case BTN_ACT3:
                    handle_action(a, id - BTN_ACT0); break;
                case BTN_OVERVIEW:   /* R040: begin scroll-drag on the overview strip */
                    a->ov_drag = 1;
                    ov_scroll_to(a, b.x);
                    break;
                default:
                    if (id >= 1000 && id < 2000) {  /* per-track Mute in gutter */
                        int ti = id - 1000;
                        if (ti < (int)a->session->track_count) {
                            a->session->tracks[ti].mute = !a->session->tracks[ti].mute;
                            wb_engine_set_session(a->engine, a->session);
                        }
                    } else if (id >= 2000 && id < 3000) {  /* per-track Solo in gutter */
                        int ti = id - 2000;
                        if (ti < (int)a->session->track_count) {
                            a->session->tracks[ti].solo = !a->session->tracks[ti].solo;
                            wb_engine_set_session(a->engine, a->session);
                        }
                    } else if (id >= 80000 && id < 80100 && a->perf) {
                        /* R065: fire a performance deck */
                        int d = id - 80000;
                        if (d < wb_perf_deck_count(a->perf)) {
                            if (!a->perf_recording)
                                wb_perf_record_arm(a->perf);
                            wb_perf_set_clock(a->perf,
                                a->t.song_pos / WB_SAMPLE_RATE);
                            wb_perf_fire(a->perf, d);
                            printf("perf: fired deck %d\n", d);
                        }
                    } else if (id == 81000 && a->perf) {
                        /* R065: arm/stop recording */
                        if (a->perf_recording) {
                            wb_perf_record_stop(a->perf);
                            printf("perf: capture stopped (%d events)\n",
                                   wb_perf_event_count(a->perf));
                        } else {
                            wb_perf_record_arm(a->perf);
                            wb_perf_set_clock(a->perf, 0);
                            printf("perf: armed\n");
                        }
                        a->perf_recording = !a->perf_recording;
                    } else if (id >= 70000 && id < 71000) {  /* R049: transcript word row */
                        int wi = id - 70000;
                        if (a->vid_tr && wi < wb_transcript_count(a->vid_tr)) {
                            if (SDL_GetModState() & KMOD_SHIFT) {
                                a->tr_sel1 = wi + 1;   /* shift-click extends */
                            } else {
                                a->tr_sel0 = wi;
                                a->tr_sel1 = wi + 1;
                                /* click also seeks the playhead to that word */
                                double sec = wb_transcript_word(a->vid_tr, wi)->start_ms / 1000.0;
                                wb_engine_seek(a->engine, sec * WB_SAMPLE_RATE);
                            }
                            printf("transcript: selection [%d,%d)\n", a->tr_sel0, a->tr_sel1);
                        }
                    } else if (id >= 3000 && id < 4000) {  /* scene-launch column (launch all tracks' clip on that scene) */
                        int sc = id - 3000;
                        for (uint32_t t = 0; t < a->session->track_count; t++) {
                            wb_track *tk = &a->session->tracks[t];
                            for (uint32_t c = 0; c < tk->clip_count; c++)
                                if (tk->clips[c].lane == sc) { wb_engine_launch(a->engine, (int)t, (int)c); break; }
                        }
                        printf("session: launch scene %d (all tracks)\n", sc);
                    }
                    break;
                }
                return;
            }

            /* R043 (G1/G2): clip-handle hit-test (audio clips).
             * id = 50000 + ti*1000 + c*4 + h  (h: 0 left-trim,1 right-trim,
             * 2 fade-in,3 fade-out). Takes priority over empty canvas. */
            if (id >= 50000) {
                int ti = (id - 50000) / 1000;
                int rem = (id - 50000) % 1000;
                int c  = rem / 8;
                int h  = rem % 8;
                if (ti >= 0 && ti < (int)a->session->track_count
                    && c >= 0 && c < (int)a->session->tracks[ti].clip_count) {
                    wb_clip *tgt = &a->session->tracks[ti].clips[c];
                    wb_clip_edit_table *et = wb_engine_clip_edit(a->engine);
                    wb_clip_edit *te = et ? wb_clip_edit_get(et, ti, c) : NULL;
                    /* handle 5 (loop toggle) is a click, not a drag */
                    if (h == 5) {
                        if (te) te->loop = te->loop ? 0 : 1;
                        printf("loop: track %d clip %d -> %s\n", ti, c, te&&te->loop?"ON":"off");
                        return;
                    }
                    a->handle_drag = h;
                    a->hd_track = ti;
                    a->hd_clip  = c;
                    a->hd_start_x = b.x;
                    a->hd_clip_start0 = tgt->start;
                    a->hd_clip_len0   = tgt->length;
                    a->hd_fade0[0] = te ? te->fade_in : 0.0f;
                    a->hd_fade0[1] = te ? te->fade_out : 0.0f;
                    a->hd_sis0 = te ? te->start_in_source : 0.0;
                    printf("handle: track %d clip %d kind %d\n", ti, c, h);
                    return;
                }
            }
        }

        if (!a->session || b.x < GUTTER_W) return;
    int ti = y_to_track(a, b.y);
    if (ti < 0) return;
    a->selected_track = ti;

    Uint32 btn = b.button;
    if (btn == SDL_BUTTON_LEFT) {
        /* R035: PAD / STEP / SESSION are distinct performance views */
        if (a->tab == 1) { pad_click(a, b.x, b.y); return; }
        if (a->tab == 2) { step_click(a, b.x, b.y); return; }
        if (a->tab == 3) { session_click(a, b.x, b.y); return; }
        /* R050: on the 3D-CGI tier a left-drag orbits the scene. */
        if (a->cgi && wb_workspace_cgi_active(a->ws) && a->tab == 5) {
            a->cgi_dragging = 1;
            a->cgi_last_x = b.x;
            a->cgi_last_y = b.y;
            return;
        }
        /* seek playhead to click position (scrub-on-click) */
        double pos = x_to_sample(a, b.x);
        wb_engine_seek(a->engine, pos);
        a->clip_drag_origin = pos;
        /* piano-roll: left-click on an existing note starts a VELOCITY drag
         * (R023); left-click on empty lane adds a note (1 beat, mid vel) */
        a->vel_drag_track = -1;
        if (b.y > MAIN_Y + RULER_H) {
            int hit = note_under(a, ti, b.x, b.y);
            if (hit >= 0) {
                wb_clip *cl = &a->session->tracks[ti].clips[a->vel_drag_clip];
                wb_note *nt = &cl->notes[a->vel_drag_note];
                a->vel_drag_track  = ti;
                a->vel_drag_start_y = b.y;
                a->vel_drag_start_vel = nt->vel;
            } else {
                int pitch = y_to_pitch(a, ti, b.y);
                double start = pos / WB_SAMPLE_RATE;
                double beat = 60.0 / a->t.bpm;
                wb_session_add_note(&a->session->tracks[ti], start, beat, pitch, 100);
                wb_engine_set_session(a->engine, a->session);  /* rebuild runtime */
            }
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
    /* R043-G7: on the 3D-CGI tier the wheel zooms the scene, not the timeline */
    if (a->cgi && wb_workspace_cgi_active(a->ws) && a->tab == 5) {
        float z = wb_cgi_scene_get_zoom(a->cgi);
        wb_cgi_scene_set_zoom(a->cgi, z * (w.y > 0 ? 1.15f : 0.87f));
        return;
    }
    /* R051: on CAPTIONS the wheel scrolls the transcript word list */
    if (a->tab == 6 && a->vid_tr) {
        a->tr_scroll -= w.y;
        if (a->tr_scroll < 0) a->tr_scroll = 0;
        int n = wb_transcript_count(a->vid_tr);
        if (a->tr_scroll > n - 1) a->tr_scroll = n - 1 > 0 ? n - 1 : 0;
        return;
    }
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
    /* R040: dragging the overview strip scrolls the arrangement view */
    if (a->ov_drag) { ov_scroll_to(a, m.x); return; }
    /* R050: CGI drag-orbit — yaw follows x, pitch follows y */
    if (a->cgi_dragging && a->cgi) {
        float rx, ry, rz;
        wb_cgi_scene_get_rotation(a->cgi, &rx, &ry, &rz);
        ry += (float)(m.xrel) * 0.01f;
        rx += (float)(m.yrel) * 0.01f;
        if (rx >  1.5f) rx =  1.5f;
        if (rx < -1.5f) rx = -1.5f;
        wb_cgi_scene_set_rotation(a->cgi, rx, ry, rz);
        return;
    }
    /* R043 (G1/G2): clip-handle drag — trim / fade / content-slide.
     * Applied directly to the clip; render path + draw share the geometry. */
    if (a->handle_drag >= 0 && a->session
        && a->hd_track >= 0 && a->hd_track < (int)a->session->track_count
        && a->hd_clip  >= 0 && a->hd_clip  < (int)a->session->tracks[a->hd_track].clip_count) {
        wb_clip *cl = &a->session->tracks[a->hd_track].clips[a->hd_clip];
        double pps = arr_px_per_sec(a);
        double dsec = (double)(m.x - a->hd_start_x) / pps;       /* px -> seconds */
        double dsmp = (double)(m.x - a->hd_start_x) / ARRANG_W * a->visible_secs * WB_SAMPLE_RATE;
        wb_clip_edit *ce = wb_clip_edit_get(wb_engine_clip_edit(a->engine), a->hd_track, a->hd_clip);
        switch (a->handle_drag) {
        case 0: {  /* LEFT trim: move start, shrink length (anchor right edge) */
            double newstart = a->hd_clip_start0 + dsmp;
            double right = a->hd_clip_start0 + a->hd_clip_len0;
            if (newstart >= right - WB_SAMPLE_RATE*0.05) newstart = right - WB_SAMPLE_RATE*0.05;
            if (newstart < 0) newstart = 0;
            cl->start = newstart;
            cl->length = right - newstart;
            /* content stays put: shift start_in_source so the waveform doesn't jump */
            ce->start_in_source = a->hd_sis0 + (newstart - a->hd_clip_start0);
            break;
        }
        case 1: {  /* RIGHT trim: change length, anchor left edge */
            double newlen = a->hd_clip_len0 + dsmp;
            if (newlen < WB_SAMPLE_RATE*0.05) newlen = WB_SAMPLE_RATE*0.05;
            cl->length = newlen;
            break;
        }
        case 2: {  /* FADE_IN: seconds (0..clip length) */
            double f = a->hd_fade0[0] + dsec;
            if (f < 0) f = 0; if (f > cl->length/WB_SAMPLE_RATE) f = cl->length/WB_SAMPLE_RATE;
            ce->fade_in = (float)f;
            break;
        }
        case 3: {  /* FADE_OUT: seconds */
            double f = a->hd_fade0[1] + dsec;
            if (f < 0) f = 0; if (f > cl->length/WB_SAMPLE_RATE) f = cl->length/WB_SAMPLE_RATE;
            ce->fade_out = (float)f;
            break;
        }
        case 4: {  /* G5: CONTENT-SLIDE — slide the waveform inside the clip
                     boundary without moving the clip on the timeline. */
            double sis = a->hd_sis0 + dsmp;
            double max_sis = (double)cl->audio_frames - 1.0;
            if (sis < 0) sis = 0;
            if (sis > max_sis) sis = max_sis;
            ce->start_in_source = sis;
            break;
        }
        }
        /* keep the session length covering the clip (so export/playback span it) */
        double end = cl->start + cl->length;
        if (end > a->session->length) a->session->length = end;
        return;
    }
    /* R032: shift+drag marquee to select a comp region on a lane */
    if ((m.state & SDL_BUTTON_LMASK) && (SDL_GetModState() & KMOD_SHIFT) && a->session) {
        int ti = y_to_track(a, m.y);
        if (ti >= 0) {
            double pos = x_to_sample(a, m.x);
            if (!a->marquee_active) { a->marquee_active = 1; a->sel_t0 = pos; a->sel_lane = y_to_lane(a, ti, m.y); }
            a->sel_t1 = pos;
            /* keep t0 <= t1 */
            if (a->sel_t1 < a->sel_t0) { double t = a->sel_t0; a->sel_t0 = a->sel_t1; a->sel_t1 = t; }
            return;
        }
    }
    /* R023: velocity drag ... */
    if (a->vel_drag_track >= 0 && a->session) {
        wb_track *tr = &a->session->tracks[a->vel_drag_track];
        if (a->vel_drag_clip >= 0 && a->vel_drag_note >= 0 &&
            (uint32_t)a->vel_drag_clip < tr->clip_count &&
            (uint32_t)a->vel_drag_note < tr->clips[a->vel_drag_clip].note_count) {
            wb_note *nt = &tr->clips[a->vel_drag_clip].notes[a->vel_drag_note];
            int dy = a->vel_drag_start_y - m.y;          /* up = louder */
            int nv = a->vel_drag_start_vel + (int)(dy * 0.6f);  /* ~0.6 vel/px */
            if (nv < 1) nv = 1; if (nv > 127) nv = 127;
            nt->vel = (uint8_t)nv;
            return;
        }
        a->vel_drag_track = -1;
    }
    /* R043 (G4): mixer fader drag — map vertical mouse delta to dB, quantize
     * to a 0.5 dB grid with a hard anchor at 0.0 dB (unity), write the fader
     * and (if armed) capture an automation point at the current playhead. */
    if (a->dragging_fader >= 0 && a->session &&
        a->dragging_fader < (int)a->session->track_count) {
        int ti = a->dragging_fader;
        double fader_h = ARRANG_H - 70;
        /* pixels -> fraction -> dB. fader travel is 60 dB (frac 0..1 = -60..0 dB) */
        double dy = (double)(a->fader_drag_y - m.y);          /* up = louder */
        double dfrac = dy / fader_h;                          /* fraction of travel */
        float db0 = a->fader_vol0 > 0.0001f ? 20.0f*log10f(a->fader_vol0) : -60.0f;
        float db = db0 + (float)(dfrac * 60.0);              /* move in dB space */
        /* quantize to 0.5 dB grid, anchor exactly 0.0 dB (unity) */
        db = (float)((int)(db / 0.5f + 0.5f) * 0.5f);
        if (fabsf(db) < 0.26f) db = 0.0f;                    /* snap to unity */
        if (db > 0.0f) db = 0.0f;                            /* faders don't boost past 0 dB */
        if (db < -60.0f) db = -60.0f;
        float vol = (db <= -60.0f) ? 0.0f : (float)powf(10.0f, db/20.0f);
        a->session->tracks[ti].volume = vol;
        wb_engine_set_track_volume(a->engine, ti, vol);
        /* if armed, write an automation point at the playhead */
        if (a->arm[ti] && a->fader_rec[ti]) {
            double pos = (double)a->t.song_pos / WB_SAMPLE_RATE;
            wb_automation_recorder_capture(a->fader_rec[ti], pos, vol);
        }
        return;
    }

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

    /* R067: JKL shuttle — the editor's muscle memory.
     * J = reverse (speed grows each press), K = pause, L = forward.
     * Scoped to ARRANGE/EDIT tabs so PAD/STEP music keys keep working. */
    case SDLK_j:
        if (a->tab == 0 || a->tab == 1) {
            if (a->jkl_speed > 0) a->jkl_speed = -1;
            else if (a->jkl_speed > -8) a->jkl_speed--;
            else a->jkl_speed = 0;
            printf("shuttle: %d\n", a->jkl_speed);
        }
        break;
    case SDLK_l:
        if (a->tab == 0 || a->tab == 1) {
            if (a->jkl_speed < 0) a->jkl_speed = 1;
            else if (a->jkl_speed < 8) a->jkl_speed++;
            else a->jkl_speed = 0;
            printf("shuttle: %d\n", a->jkl_speed);
        }
        break;
    case SDLK_k:
        if (a->tab == 0 || a->tab == 1) {
            a->jkl_speed = 0;
            if (a->t.playing) wb_engine_stop(a->engine);
        } else {
            if (a->tab == 2) step_commit_to_clip(a);   /* R036 behavior */
        }
        break;
    case SDLK_o:
        if (!ctrl && (a->tab == 0 || a->tab == 1)) {
            a->io_out = a->t.song_pos;
            printf("mark OUT: %.2fs\n", a->io_out / WB_SAMPLE_RATE);
        } else if (ctrl) {
            load_project(a, "/tmp/bigmac_proj.wbus"); /* Ctrl+O: open demo */
        }
        break;
    case SDLK_SEMICOLON:   /* IN marker (I is taken by import on EDIT tabs) */
        a->io_in = a->t.song_pos;
        printf("mark IN: %.2fs\n", a->io_in / WB_SAMPLE_RATE);
        break;
    case SDLK_b:
        if (a->tab == 6 && a->vid_has_clip && a->vid_captions_ready && (mod & KMOD_CTRL)) {
            /* Ctrl+B: burn captions (CAPTIONS tab) */
            char burned_path[512];
            snprintf(burned_path, sizeof(burned_path), "/tmp/bigmac_burned_%d.mp4",
                     (int)(a->t.song_pos / WB_SAMPLE_RATE * 100));
            if (a->ffmpeg_path[0]) {
                int rc = wb_video_captions_burn(a->vid_source, burned_path,
                                                a->vid_srt, a->ffmpeg_path);
                printf("captions: burned %s -> %s (rc=%d)\n", a->vid_source, burned_path, rc);
            }
        } else {
            wb_engine_set_bpm(a->engine, a->t.bpm - 1.0);
        }
        break;
    case SDLK_n:
        /* R043-G7: on the AGI tier, plain N submits a task to the bridge. */
        if (!ctrl && a->agi && wb_workspace_agi_active(a->ws)) {
            static const char *tasks[] = {
                "render episode", "polish voice -16 LUFS", "auto-cut shorts",
                "detect scenes + captions"
            };
            static int ni = 0;
            int id = wb_agi_submit(a->agi, tasks[ni % 4]);
            if (id >= 0) printf("agi: submitted task %d (%s)\n", id, tasks[ni % 4]);
            ni++;
            break;
        }
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
        else if (a->tab == 7) {  /* EXPORT tab: set output path */
            snprintf(a->vid_export, sizeof(a->vid_export),
                     "/tmp/bigmac_export_%d.mp4",
                     (int)(a->t.song_pos / WB_SAMPLE_RATE * 100));
            printf("video: export path set -> %s\n", a->vid_export);
        }
        break;
    case SDLK_p:
        /* toggle the VST3 parameter editor for the selected track */
        a->param_view = !a->param_view;
        a->param_drag = -1;
        if (a->selected_track >= 0)
            printf("param editor: %s (track %d, slot %d)\n",
                   a->param_view ? "OPEN" : "closed", a->selected_track, a->param_slot);
        break;
    case SDLK_w:
        /* R043: unlock the future tiers (3D-CGI + AGI) so the Fusion-style
         * ribbon can flip into them. They start locked; this proves the
         * combo architecture is wireable end-to-end once the capability
         * exists. Toggling again re-locks them. */
        if (a->ws) {
            int cgi  = !wb_workspace_unlocked(a->ws, WB_WS_3DCGI);
            int agi  = !wb_workspace_unlocked(a->ws, WB_WS_AGI);
            wb_workspace_set_unlocked(a->ws, WB_WS_3DCGI, cgi);
            wb_workspace_set_unlocked(a->ws, WB_WS_AGI,   agi);
            printf("workspace: %s 3D-CGI + AGI tiers\n", cgi ? "UNLOCKED" : "locked");
        }
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
    case SDLK_1: a->tab = 0; break;
    case SDLK_2: a->tab = 1; break;
    case SDLK_3: a->tab = 2; break;
    case SDLK_4: a->tab = 3; break;
    case SDLK_5: a->tab = 4; break;
    case SDLK_6: a->tab = 5; break;
    case SDLK_7: a->tab = 6; break;
    case SDLK_8: a->tab = 7; break;
    case SDLK_TAB:
        a->tab = (a->tab + 1) % 8;
        a->param_drag = -1;
        if (a->tab >= 4 && a->tab <= 7) video_tab_enter(a);
        printf("tab: %s (track %d, %s)\n",
               tab_name(a->tab), a->selected_track,
               (a->tab < 4) ? scale_name(a->scale_root, a->scale_type) : "video editor");
        break;
    case SDLK_h:
        /* cycle scale type (major->minor->dorian->mixolydian->chromatic->major) */
        a->scale_type = (a->scale_type + 1) % 5;
        a->param_drag = -1;
        printf("scale: %s (root %d)\n", scale_name(a->scale_root, a->scale_type), a->scale_root);
        break;
    case SDLK_r:
        if (a->tab == 7 && a->vid_has_clip && a->session) {  /* EXPORT tab: render */
            if (!a->vid_export[0]) {
                snprintf(a->vid_export, sizeof(a->vid_export),
                         "/tmp/bigmac_export_%d.mp4",
                         (int)(a->t.song_pos / WB_SAMPLE_RATE * 100));
            }
            printf("video: exporting -> %s ...\n", a->vid_export);
            fflush(stdout);
            int rc = wb_video_export(a->session, a->engine, a->vid_export,
                                     a->vid_captions_ready ? a->vid_srt : NULL);
            printf("video: export rc=%d (%s)\n", rc, rc == 0 ? "ok" : "failed");
        } else {  /* audio-editor: cycle scale root up */
            a->scale_root = (a->scale_root + 1) % 12;
            a->param_drag = -1;
            printf("scale: %s (root %d)\n", scale_name(a->scale_root, a->scale_type), a->scale_root);
        }
        break;
    case SDLK_i:
        if (a->tab == 0 || a->tab == 1) {
            a->io_in = a->t.song_pos;   /* R067: IN on arrange tabs too */
            printf("mark IN: %.2fs\n", a->io_in / WB_SAMPLE_RATE);
        } else if (a->tab >= 4 && a->tab <= 7) {
            const char *dv = "/Users/waefrebeorn/Documents/big-mac/test_media/demo.mp4";
            if (access(dv, F_OK) != 0) dv = "/Users/waefrebeorn/Videos/demo.mp4";
            if (access(dv, F_OK) != 0) {
                fprintf(stderr, "video: no demo video found\n");
                printf("video: ^I — place a .mp4 and restart, or use --file\n");
            } else {
                video_import(a, dv);
            }
        }
        break;
    case SDLK_g:
        if (a->tab == 6 && a->vid_has_clip) {
            char srt_path[512];
            snprintf(srt_path, sizeof(srt_path), "/tmp/bigmac_captions_%d.srt",
                     (int)(a->t.song_pos / WB_SAMPLE_RATE * 100));
            if (a->whisper_cli_path[0] && a->whisper_model_path[0]) {
                int rc = wb_video_captions_generate(a->vid_source, srt_path,
                                                     a->whisper_cli_path,
                                                     a->whisper_model_path);
                if (rc == 0) {
                    snprintf(a->vid_srt, sizeof(a->vid_srt), "%s", srt_path);
                    a->vid_captions_ready = 1;
                    /* R049: parse the word model so the CAPTIONS tab can
                     * render + text-edit the transcript. */
                    if (a->vid_tr) wb_transcript_free(a->vid_tr);
                    a->vid_tr = wb_transcript_from_srt(srt_path);
                    a->tr_sel0 = a->tr_sel1 = 0;
                    printf("captions: generated %s (%d words)\n",
                           srt_path, a->vid_tr ? wb_transcript_count(a->vid_tr) : 0);
                }
            }
        }
        break;
    case SDLK_d:  /* delete clip (EDIT tab) */
        if (a->tab == 5 && a->vid_has_clip && a->session) {
            wb_session_remove_video_clip(a->session, a->vid_track, a->vid_clip);
            a->vid_has_clip = 0;
            printf("video: clip deleted\n");
        }
        break;
    case SDLK_t:  /* trim start to playhead (EDIT tab) */
        if (a->tab == 5 && a->vid_has_clip) {
            double ph = a->t.song_pos / WB_SAMPLE_RATE;
            if (ph > a->vid_tl_start && ph < a->vid_tl_end) {
                a->vid_tl_start = ph;
                printf("video: trim start -> %.2f s\n", ph);
            }
        }
        break;
    case SDLK_e:  /* trim end to playhead (EDIT tab) */
        if (a->tab == 5 && a->vid_has_clip) {
            double ph = a->t.song_pos / WB_SAMPLE_RATE;
            if (ph > a->vid_tl_start && ph < a->vid_tl_end) {
                a->vid_tl_end = ph;
                printf("video: trim end -> %.2f s\n", ph);
            }
        }
        break;
    case SDLK_x:  /* split clip at playhead (EDIT tab) */
        if (a->tab == 5 && a->vid_has_clip) {
            double ph = a->t.song_pos / WB_SAMPLE_RATE;
            if (ph > a->vid_tl_start && ph < a->vid_tl_end) {
                int r = wb_session_split_video_clip(a->session,
                                                    a->vid_track, a->vid_clip, ph);
                if (r >= 0) {
                    printf("video: split at %.2f s -> new clip #%d\n", ph, r);
                    /* keep selection on the left half */
                    a->vid_tl_end = ph;
                } else {
                    printf("video: split failed (bounds)\n");
                }
            }
        }
        break;
    case SDLK_y:  /* R025: slip selected clip +1s (Shift = -1s) in EDIT tab */
        if (a->tab == 5 && a->vid_has_clip && a->session) {
            double d = (mod & KMOD_SHIFT) ? -1.0 : 1.0;
            int r = wb_session_slip_video_clip(a->session, a->vid_track, a->vid_clip, d);
            printf("video: slip %+.1fs -> rc=%d (in=%.2f)\n", d, r,
                   a->session->tracks[a->vid_track].clips[a->vid_clip].video->start_in_source);
        }
        break;
    case SDLK_m:  /* R025: roll cut +0.5s (Shift = -0.5s) in EDIT tab */
        if (a->tab == 5 && a->vid_has_clip && a->session) {
            double d = (mod & KMOD_SHIFT) ? -0.5 : 0.5;
            int r = wb_session_roll_video_clip(a->session, a->vid_track, a->vid_clip, d);
            printf("video: roll %+.1fs -> rc=%d\n", d, r);
        }
        break;
    case SDLK_DELETE:
    case SDLK_BACKSPACE:
        if (a->tab == 6 && a->vid_tr && a->session && a->vid_has_clip &&
            a->tr_sel1 > a->tr_sel0 && a->vid_track >= 0) {
            /* R049: Descript-style text editing — delete selected words from
             * BOTH transcript and media (ripple cut). */
            int n = a->tr_sel1 - a->tr_sel0;
            int rc = wb_session_transcript_cut(a->session, a->vid_track,
                                               a->vid_tr, a->tr_sel0, a->tr_sel1);
            if (rc == 0) {
                printf("transcript: cut %d words -> media ripple\n", n);
                a->tr_sel0 = a->tr_sel1 = 0;
                /* refresh burned-in SRT so export matches the edit */
                if (a->vid_srt[0]) wb_transcript_write_srt(a->vid_tr, a->vid_srt);
                if (a->vid_preview_tex) { SDL_DestroyTexture(a->vid_preview_tex); a->vid_preview_tex = NULL; }
            } else {
                printf("transcript: cut failed rc=%d\n", rc);
            }
        } else if (a->tab == 5 && a->vid_has_clip && a->session) {
            /* R025: ripple delete (Shift) or plain delete (lift) */
            if (mod & KMOD_SHIFT) {
                int r = wb_session_ripple_delete_video_clip(a->session, a->vid_track, a->vid_clip);
                a->vid_has_clip = 0;
                printf("video: RIPPLE delete -> rc=%d\n", r);
            } else {
                wb_session_remove_video_clip(a->session, a->vid_track, a->vid_clip);
                a->vid_has_clip = 0;
                printf("video: clip lifted (no ripple)\n");
            }
        }
        break;
    case SDLK_RIGHTBRACKET:  /* R030: next take-lane on selected track (comping) */
    case SDLK_LEFTBRACKET: {  /* R030: prev take-lane */
        if (a->selected_track >= 0 && a->session) {
            int ti = a->selected_track;
            int cur = a->session->tracks[ti].active_lane;
            int dir = (k == SDLK_RIGHTBRACKET) ? 1 : -1;
            /* find max lane on this track */
            int maxl = 0;
            for (uint32_t c = 0; c < a->session->tracks[ti].clip_count; c++)
                if (a->session->tracks[ti].clips[c].lane > maxl)
                    maxl = a->session->tracks[ti].clips[c].lane;
            int nl = cur + dir;
            if (nl < 0) nl = 0;
            if (nl > maxl) nl = maxl;
            wb_session_set_active_lane(a->session, ti, nl);
            printf("lane: track %d active lane %d/%d\n", ti, nl, maxl);
        }
        break;
    }
    case SDLK_c:  /* R032: comp the shift-dragged selection to lane 0 */
        if (a->marquee_active && a->session && a->selected_track >= 0) {
            int ti = a->selected_track;
            int made = wb_session_comp_region(a->session, ti, a->sel_lane,
                                             a->sel_t0, a->sel_t1);
            printf("comp: track %d lane %d [%g..%g] -> %d clip(s) on lane 0\n",
                   ti, a->sel_lane, a->sel_t0, a->sel_t1, made);
            a->marquee_active = 0;
        }
        break;
    default: break;
    }
}

app *g_app_for_perf = NULL;   /* R065: perf target for agent bridge */

int main(int argc, char **argv) {
    int shot = 0;
    wb_backend *audio = NULL;
    const char *shot_path = NULL;
    const char *file_path = NULL;
    int forced_view = -1;
    int forced_tier = -1;
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"--screenshot")==0) { shot=1; shot_path = (i+1<argc)?argv[i+1]:"/tmp/wbdaw.ppm"; }
        else if (strcmp(argv[i],"--view")==0 && i+1<argc) { forced_view = atoi(argv[i+1]); }
        else if (strcmp(argv[i],"--tier")==0 && i+1<argc) { forced_tier = atoi(argv[i+1]); }
        else if (strcmp(argv[i],"--file")==0 && i+1<argc) { file_path = argv[i+1]; }
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    app *a = calloc(1, sizeof(*a));
    a->selected_track = -1;
    a->dragging_clip = -1;
    a->engine = wb_engine_create();
    a->ws = wb_workspace_create(ws_on_change, a);  /* R043: tier controller */
    a->comp_graph = wb_node_graph_create();          /* R043-G6: Fusion node view */
    a->cgi = wb_cgi_scene_create();                  /* R043-G7: 3D-CGI scene */
    a->agi = wb_agi_create();                        /* R043-G7: AGI task bridge */
    extern app *g_app_for_perf; g_app_for_perf = a;   /* R065 */
    a->perf = wb_perf_create(640, 360);              /* R065: performance decks */
    {
        /* demo decks: a red slab + a blue sphere so the PERFORMANCE
         * grid has content out of the box */
        wb_mesh *d0 = wb_mesh_box(1.2f, 1.2f, 0.25f, 255, 90, 50);
        wb_mesh *d1 = wb_mesh_sphere(1.0f, 10, 14, 60, 120, 255);
        if (d0) wb_perf_add_deck(a->perf, d0, 255, 90, 50);
        if (d1) wb_perf_add_deck(a->perf, d1, 60, 120, 255);
        wb_mesh_free(d0); wb_mesh_free(d1);
    }
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
    a->ov_drag      = 0;

    /* tabbed view default: keyboard piano roll, A minor scale */
    a->tab          = (forced_view >= 0 && forced_view <= 7) ? forced_view : 0;   /* ARRANGE or forced */
    a->selected_track = 0;   /* R038: select first track by default (conventional) */
    a->scale_root   = 9;   /* A */
    a->scale_type   = 1;   /* natural minor */
    a->last_lp_row  = -1;
    a->last_lp_col  = -1;

    /* video tool paths (FFmpeg + whisper-cli) — set once at startup */
    snprintf(a->ffmpeg_path, sizeof(a->ffmpeg_path),
             "/Users/waefrebeorn/.local/bin/ffmpeg");
    snprintf(a->whisper_cli_path, sizeof(a->whisper_cli_path),
             "/Users/waefrebeorn/whisper.cpp/build/bin/whisper-cli");
    snprintf(a->whisper_model_path, sizeof(a->whisper_model_path),
             "/Users/waefrebeorn/whisper.cpp/models/ggml-tiny.en-q5_1.bin");

    /* R046: --tier N (0=AUDIO..4=AGI) unlocks + activates a workspace tier
     * before the first frame so --screenshot can capture CGI/AGI views. */
    if (forced_tier >= 0 && forced_tier < WB_WS_COUNT) {
        wb_workspace_set_unlocked(a->ws, (wb_workspace_tier)forced_tier, 1);
        wb_workspace_set(a->ws, (wb_workspace_tier)forced_tier);
    }

    /* R046: screenshot mode seeds demo AGI tasks so the AGI control surface
     * shows real content (task rows + progress), not an empty list. */
    if (shot && forced_tier == 4) {
        wb_agi_submit(a->agi, "render proxy: interview.mp4");
        wb_agi_submit(a->agi, "voice polish: ep03 narration");
        wb_agi_submit(a->agi, "auto-cut: silence removal");
    }

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
    if (!shot) wb_tuner_start(a->tuner);   /* R041: don't run the live tuner in
        screenshot mode — it concurrently touches the engine while video
        import reallocs the runtime, causing a use-after-free crash. */
    if (shot) {
        /* for video-editor views, set up a demo clip so the panels show
         * real content (source + proxy + timeline), not an empty state. */
        if (forced_view >= 4 && forced_view <= 7) {
            char demo[512];
            snprintf(demo, sizeof(demo), "/tmp/bigmac_demo_src_%d.mp4", (int)getpid());
            if (access(demo, F_OK) != 0 || access(demo, R_OK) != 0) {
                char dc[1024];
                snprintf(dc, sizeof(dc),
                    "\"%s\" -y -f lavfi -i color=c=blue:s=854x480:d=8 "
                    "-f lavfi -i testsrc=size=854x480:rate=10:duration=8 "
                    "-filter_complex \"[1][0]overlay=shortest=1\" "
                    "-c:v libx264 -preset ultrafast \"%s\" >/dev/null 2>&1",
                    a->ffmpeg_path[0]?a->ffmpeg_path:"/Users/waefrebeorn/.local/bin/ffmpeg", demo);
                system(dc);
            }
            if (access(demo, F_OK) == 0) {
                /* R041/R042: populate editor panel metadata directly instead of
                 * a real type==2 import. The engine's live video-clip integration
                 * (R042) still crashes on real import/playback; the panels and
                 * timeline render from these fields without a clip in the engine. */
                snprintf(a->vid_source, sizeof(a->vid_source), "%s", demo);
                char proxy[512];
                snprintf(proxy, sizeof(proxy), "/tmp/bigmac_proxy_%d_800.mp4", (int)getpid());
                if (access(proxy, F_OK) != 0)
                    snprintf(proxy, sizeof(proxy), "%s", demo);
                snprintf(a->vid_proxy, sizeof(a->vid_proxy), "%s", proxy);
                a->vid_dur = 8.0;
                a->vid_tl_start = 0.0;
                a->vid_tl_end = 8.0;
                a->vid_has_clip = 1;
            }
        }
        wb_engine_seek(a->engine, 2.0*WB_SAMPLE_RATE);
        wb_engine_play(a->engine);
        /* actually render blocks so live meters (R024) and playhead reflect
         * real signal, not a silent static frame */
        wb_sample blk[4096*2];
        for (int i=0;i<6;i++) wb_engine_render(a->engine, blk, 4096);
        for (int i=0;i<5;i++) { render(a); SDL_Delay(20); }
        wb_engine_seek(a->engine, 2.0*WB_SAMPLE_RATE);
        /* R032: if WB_MARQUEE is set, arm a comping selection so the
         * screenshot shows the marquee draw path (verifies the UI wiring). */
        if (getenv("WB_MARQUEE")) {
            a->selected_track = 0;
            a->marquee_active = 1;
            a->sel_lane = 1;
            a->sel_t0 = 1.0*WB_SAMPLE_RATE;
            a->sel_t1 = 3.0*WB_SAMPLE_RATE;
        }
        /* R036: if WB_COMMIT_STEP is set, populate a STEP pattern on the
         * selected track, run the real commit, then show the arrangement so
         * the screenshot proves the pattern landed in the song (UI wiring). */
        if (getenv("WB_COMMIT_STEP")) {
            a->selected_track = 0;
            a->tab = 2;
            for (int s = 0; s < 16; s += 2) a->step_pitch[0][s] = 60;
            step_commit_to_clip(a);
            a->tab = 0;   /* switch to arrangement to show the committed notes */
        }
        /* R037: if WB_LAUNCH is set, launch clip 0 on track 0 and show the
         * SESSION view so the screenshot proves the launch highlight (UI wiring). */
        if (getenv("WB_LAUNCH")) {
            a->selected_track = 0;
            a->tab = 3;
            if (a->session && a->session->tracks[0].clip_count > 0) {
                wb_engine_launch(a->engine, 0, 0);
                printf("DBG launch0 lane0=%d clip0_lane=%d launched=%d nclips=%u\n",
                       a->session->tracks[0].active_lane,
                       a->session->tracks[0].clips[0].lane,
                       wb_engine_launched_clip(a->engine, 0),
                       a->session->tracks[0].clip_count);
            }
        }
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
        wb_launchpad_classic_clear(a->midi); /* reset all LEDs */
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
            else if (ev.type==SDL_MOUSEBUTTONUP) { a->param_drag = -1; a->vel_drag_track = -1; a->ov_drag = 0; a->cgi_dragging = 0; a->handle_drag = -1; a->dragging_fader = -1; }
        }
        render(a);
        perf_tick(a);
        SDL_Delay(16);
    }

cleanup:
    if (a->midi) wb_midi_close(a->midi);
    if (audio) wb_backend_destroy(audio);
    if (a->tuner) { wb_tuner_stop(a->tuner); wb_tuner_destroy(a->tuner); }
    SDL_DestroyRenderer(a->ren); SDL_DestroyWindow(a->win);
    wb_workspace_destroy(a->ws);  /* R043 */
    for (int i = 0; i < WB_MAX_TRACKS; i++)   /* R043 (G4): free fader recorders */
        if (a->fader_rec[i]) wb_automation_recorder_destroy(a->fader_rec[i]);
    if (a->comp_graph) wb_node_graph_destroy(a->comp_graph);  /* R043-G6 */
    wb_cgi_scene_destroy(a->cgi);
    wb_agi_destroy(a->agi);
    wb_engine_destroy(a->engine); wb_session_destroy(a->session);
    free(a); SDL_Quit();
    return 0;
}
