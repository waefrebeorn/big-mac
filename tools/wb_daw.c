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
#include "wbus/wbus_agent.h"
#include "wbus/wbus_wavcache.h"
#include "wbus/wbus_capture.h"      /* G93/G94 */
#include "wbus_precision.h"   /* Wave2 lane B: G15/G16/G65/G66 */
#include "wbus/wbus_export_job.h"   /* G38 background render queue */
#include "wbus_perfclip.h"
#include "wbus_cgiexport.h"
#include "wbus_delivery.h"
#include "wb_internal.h"
#include "wb_ui.h"
#include <time.h>
#include <sys/stat.h>

/* 480p proxy dimensions — mirror wb_video.c */
#ifndef PROXY_SCALE_W
#define PROXY_SCALE_W 854
#endif
#ifndef PROXY_SCALE_H
#define PROXY_SCALE_H 480
#endif

/* ---- helpers ----------------------------------------------------------- */
static char g_scale_name_buf[32];   /* scale_name output buffer            */

static const char *tab_name(int t) {
    static const char *names[] = {
        "ARRANGE", "PAD", "STEP", "SESSION",
        "MEDIA", "EDIT", "CAPTIONS", "EXPORT"
    };
    return (t >= 0 && t < 8) ? names[t] : "KEYS";
}

static const char *scale_name(int root, int type) {
    static const char *roots[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const char *types[] = {"major","minor","dorian","mixolydian","phrygian"};
    const char *ts = (type >= 0 && type < 5) ? types[type] : types[0];
    snprintf(g_scale_name_buf, sizeof(g_scale_name_buf), "%s %s", roots[root % 12], ts);
    return g_scale_name_buf;
}

/* (void)type; kept for callers that only need the root glyph */
static const char *scale_root_name(int root) {
    static const char *roots[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    return roots[root % 12];
}

/* ---- G81: chord tones (engine-owned in wb_midi_coremidi.c, see wbus_midi.h) */
/* Clamp velocity/probability helpers (engine-owned in wb_step.h) */

/* ---- geometry --------------------------------------------------------- */

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
    int scale_type;          /* 0=major 1=minor 2=dorian 3=mixolydian 4=phrygian */
    int scale_lock;          /* G80: 1 = draw/snap to-key notes only */
    int chord_mode;          /* G81: 0=off 1=triad 2=7th 3=9th */
    int step_vel[WB_MAX_TRACKS][16];   /* G87: per-step velocity 0..127 */
    int step_prob[WB_MAX_TRACKS][16];  /* G88: per-step probability 0..100 */
    int step_sel;            /* G87: step selected for velocity edit (-1=none) */
    int fx_add_cycle;        /* G31: +FX palette cycle position */
    /* G24: keyframe graph editor (Gain node param, FUSION view) */
    wb_param_track *kf_track;   /* owned here; bound to the graph's Gain node */
    int kf_drag;                 /* -1 none, else key index being dragged */
    double kf_drag_t0;           /* key time at drag start */
    int vel_drag_step;       /* G87: vertical drag active on this step (-1=none) */
    int  prefs_visible;      /* G51: preferences overlay toggle */
    int  reduced_motion;     /* G62: WB_REDUCED_MOTION=1 disables flashes */
    /* G44: title tool */
    char title_text[128];
    double title_in, title_out;   /* seconds */
    int  title_pos;               /* 0 lower-third, 1 centered */
    int  title_entry;             /* 1 = capture keys into title_text */
    /* G35: MIDI learn — CC number -> target. target: 0=master vol,
     * 1=selected track vol, 2=tempo, 3=insert slot 1 param 0 (selected tr). */
    int   midi_learn_armed;  /* 1 = next CC received binds */
    int   midi_learn_target; /* target id to bind when armed */
    struct { int cc; int target; } midi_map[16];
    int   midi_map_n;
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
    int  export_codec_h264;  /* EXPORT tab codec toggle: 1=H264, 0=ProRes */
    char last_status[128];   /* one-line user feedback for the status bar */
    time_t last_autosave;    /* G57: epoch of last autosave (0 = save soon) */
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
    /* Wave2 lane B: G15 trim mode + G16 razor + G66 drop mode */
    int      trim_mode;      /* 1 = TRIM MODE active (EDIT tab) */
    int      trim_edge;      /* 0 = in-point, 1 = out-point */
    int      trim_clip;      /* clip index on vid_track being trimmed */
    int      razor_on;       /* G16: next timeline click splits (then exits) */
    int      drop_insert;    /* G66: 0 = OVERWRITE (default), 1 = INSERT */
    SDL_Texture *trim_tex[2];        /* G65 cached two-up frame textures */
    char     trim_tex_key[2][640];   /* proxy path + timecode per texture */
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

    /* Wave1 G01/G02: in-GUI media browser state */
    int  browser_open;                       /* 1 while the browser overlay shows */
    int  browser_scroll;                     /* first visible row */
    int  browser_count;                      /* entries in browser_paths */
    char browser_paths[128][WB_IMPORT_PATH_MAX];

    /* Wave1 G08: undo/redo history (session snapshots) */
    wb_undo *undo;

    /* Wave1 lane C: G38 render queue / G39 range / G40 resolution,
     * G93 capture log, G94 session-record-to-arrangement */
    wb_caplog    *caplog;            /* rolling log of played notes */
    double        capture_window;    /* CAPTURE keeps the last N seconds */
    wb_launchrec *lrec;              /* launcher state recorder */
    int           launchrec_armed;   /* REC ARR toggle on SESSION tab */
    wb_export_job ejob;              /* one-slot background export queue */
    /* G43/G54: watch folder — new media auto-imports; a .wbus drop
     * triggers an auto-render of the current session (AME model). */
    char     watch_dir[256];
    time_t   watch_last_poll;
    unsigned watch_seen_crc;         /* xor of path hashes seen last scan */
    /* G53: batch render matrix — per-marker-region jobs queued sequentially */
    struct {
        int   n;                     /* queued job count (0 = batch idle) */
        int   i;                     /* next job index */
        double rs[16], rd[16];       /* per-job range (seconds; -1 = whole) */
        char  out[16][400];          /* per-job output paths */
    } batch;
    /* G90: song mode — chain of scene indices (SESSION lanes) played in
     * order, one scene per `song_bars` bars. 0 = idle. */
    int    song_chain[16];
    /* G95: per-scene follow actions (Live 11+ two-action chance model).
     * fa_action: 0=next,1=jump,2=other(random),3=fill(re-self),4=legato
     * fa_chance: 0..100 % chance the action fires; else fall through to next.
     * fa_target: scene index for JUMP. */
    int    fa_action[16];
    int    fa_chance[16];
    int    fa_target[16];
    int    song_len;                 /* entries used (0 = idle) */
    int    song_pos;                 /* current chain index */
    int    song_bars;                /* bars per step (default 1) */
    int    song_last_bar;            /* bar count at last advance */
    int  export_range_mode;          /* 0 = WHOLE, 1 = IN..playhead */
    int  export_res_h;               /* 0/1080 native, 480, 720 */
    int  delivery_profile_idx;       /* G52: index into g_delivery_cycle */
    /* G09: arrangement-gutter track management */
    int  rename_armed;               /* typing appends to the track name */
    int  rename_track;               /* track being renamed */
    int last_click_track;           /* double-click detection */
    Uint32 last_click_ms;
    /* ---- G10: ruler loop brace + snap toggle -------------------------- */
    int snap_on;             /* G10: quantize edits to the grid */
    double ruler_in;         /* G10: IN mark (samples), <=0 unset */
    double ruler_out;        /* G10: OUT mark (samples), <=0 unset */
    int    ruler_drag;       /* G10: 0=none 1=drag-IN 2=drag-OUT */
} app;

/* G52: EXPORT-tab delivery preset cycle (names map to wb_delivery profiles;
 * "BROADCAST" is the friendly alias for the EBU-R128 profile). */
static const char *g_delivery_cycle[] = { "YOUTUBE", "NETFLIX", "BROADCAST", "A85", "PODCAST" };
#define G_DELIVERY_N ((int)(sizeof g_delivery_cycle / sizeof g_delivery_cycle[0]))
static const wb_delivery_profile *daw_delivery_profile(int idx) {
    if (idx < 0 || idx >= G_DELIVERY_N) idx = 0;
    const char *nm = g_delivery_cycle[idx];
    if (!strcmp(nm, "BROADCAST")) nm = "EBU-R128";
    if (!strcmp(nm, "A85")) nm = "ATSC-A85";
    return wb_delivery_profile_by_name(nm);
}

/* forward decl: logging note wrapper used by the MIDI thread too */
static void daw_note(app *a, int track, uint8_t pitch, uint8_t vel);

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
    BTN_UNDO, BTN_REDO,                       /* Wave1 G08: transport undo/redo */
    BTN_BROWSE, BTN_BROWSER_CANCEL,           /* Wave1 G01: media browser */
    BTN_SWING_M, BTN_SWING_P,                 /* Wave1 G89: swing -/+ */
    BTN_SCALE_ROOT, BTN_SCALE_TYPE,           /* Wave3 lane A: G80 */
    BTN_LOCK, BTN_CHORD,                      /* G80 scale-lock, G81 chord */
    BTN_SNAP,                                 /* G10: snap toggle */
    BTN_COUNT
};
/* ids 90000+i are the media-browser file rows (Wave1 G01) */
#define BTN_BROWSER_ROW0 90000
typedef struct { int id; SDL_Rect r; } click_region;
#define REGION_MAX 8192   /* browser rows + all buttons + mixer send ids */
static click_region g_regions[REGION_MAX];
static int g_nregions = 0;
static void region_reset(void) { g_nregions = 0; }
static void region_add(int id, int x, int y, int w, int h) {
    if (g_nregions >= REGION_MAX) return;
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

/* ---- G49/G60: accessibility label registry ------------------------------ */
/* Every ui_button call registers (id, label, rect). The registry is the
 * semantic map a screen reader / AX bridge consumes; dump it to
 * /tmp/bigmac_ax.json with Ctrl+Shift+A for assistive tooling and tests. */
typedef struct { int id; char label[64]; int x, y, w, h; int active; } ax_entry;
static ax_entry g_ax[256];
static int g_ax_n = 0;
static void ax_register(int id, const char *label, int x, int y, int w, int h,
                        int active) {
    if (id <= 0) return;                       /* decorative */
    for (int i = 0; i < g_ax_n; i++)
        if (g_ax[i].id == id) {
            snprintf(g_ax[i].label, sizeof(g_ax[i].label), "%.60s", label ? label : "");
            g_ax[i].x = x; g_ax[i].y = y; g_ax[i].w = w; g_ax[i].h = h;
            g_ax[i].active = active;
            return;
        }
    if (g_ax_n >= 256) return;
    ax_entry *e = &g_ax[g_ax_n++];
    e->id = id;
    snprintf(e->label, sizeof(e->label), "%.60s", label ? label : "");
    e->x = x; e->y = y; e->w = w; e->h = h; e->active = active;
}
static void ax_dump(void) {
    FILE *f = fopen("/tmp/bigmac_ax.json", "w");
    if (!f) return;
    fprintf(f, "[\n");
    for (int i = 0; i < g_ax_n; i++) {
        fprintf(f,
            "  {\"id\":%d,\"label\":\"%s\",\"rect\":[%d,%d,%d,%d],\"state\":\"%s\"}%s\n",
            g_ax[i].id, g_ax[i].label, g_ax[i].x, g_ax[i].y,
            g_ax[i].w, g_ax[i].h,
            g_ax[i].active ? "on" : "off",
            i + 1 < g_ax_n ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
}

static void ui_button(SDL_Renderer *r, int id, int x, int y, int w, int h,
                      const char *label, int active) {
    region_add(id, x, y, w, h);
    ax_register(id, label, x, y, w, h, active);
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
    /* Wave1 G08: undo/redo — lit when history is available in that direction */
    ui_button(a->ren, BTN_UNDO, 208, by, 34, bh, "UNDO",
              a->undo ? (wb_undo_depth(a->undo) > 0) : 0);
    ui_button(a->ren, BTN_REDO, 246, by, 34, bh, "REDO",
              a->undo ? (wb_undo_redo_depth(a->undo) > 0) : 0);

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

    /* G10: SNAP + loop-brace status. Snap lit when quantize-on; loop shows
     * IN..OUT range when armed. */
    ui_button(a->ren, BTN_SNAP, 560, by, 36, bh, "SNAP", a->snap_on);
    if (a->loop_on && a->ruler_in > 0 && a->ruler_out > a->ruler_in) {
        char lb[48]; snprintf(lb,sizeof(lb),
            "LOOP %.2f–%.2f s", a->ruler_in/WB_SAMPLE_RATE, a->ruler_out/WB_SAMPLE_RATE);
        wb_ui_draw_text(a->ren, WIN_W-MIXER_W-90, 34, lb, 1, C_MUTE);
    }
}

/* ---- arrangement ------------------------------------------------------ */
static void draw_ruler(app *a) {
    SDL_Rect rr = { GUTTER_W, MAIN_Y, ARRANG_W, RULER_H };
    setc(a->ren, C_PANEL2); SDL_RenderFillRect(a->ren, &rr);

    /* bar numbers (respect zoom/scroll view window).
     * The loop steps one BEAT per iteration; a bar is time_sig_num beats,
     * so only label beats that fall on bar boundaries (b % beats_per_bar==0)
     * and print the BAR number — labeling every beat was numbering the
     * timeline beats-per-bar times too fast. */
    int beats_per_bar = (a->session && a->session->time_sig_num > 0)
                        ? a->session->time_sig_num : 4;
    double bps = a->t.bpm / 60.0;
    double v0 = a->view_start / WB_SAMPLE_RATE;
    double vis = a->visible_secs;
    int b0 = (int)(v0 * bps);
    for (int b = b0; (b - b0) * (1.0/bps) < vis + 1.0/bps; b++) {
        double sec = b / bps;
        int x = GUTTER_W + (int)((sec - v0) * (ARRANG_W / vis));
        if (x < GUTTER_W) continue;
        int on_bar = (b % beats_per_bar) == 0;
        setc(a->ren, on_bar ? C_TEXT_DIM : C_GRID);
        SDL_RenderDrawLine(a->ren, x, MAIN_Y, x, MAIN_Y+RULER_H);
        if (on_bar) {
            char bar[8]; snprintf(bar,sizeof(bar),"%d", b / beats_per_bar + 1);
            wb_ui_draw_text(a->ren, x+3, MAIN_Y+6, bar, 1, C_TEXT_DIM);
        }
    }
    /* G10: loop-brace tabs on the ruler (drawn when loop is armed) */
    if (a->loop_on) {
        if (a->ruler_in > 0) {
            int xi = GUTTER_W + (int)((a->ruler_in/WB_SAMPLE_RATE - a->view_start/WB_SAMPLE_RATE)*(ARRANG_W/a->visible_secs));
            setc(a->ren, C_MUTE); SDL_RenderDrawLine(a->ren, xi, MAIN_Y, xi, MAIN_Y+RULER_H+3);
            region_add(70001, xi-4, MAIN_Y, 8, RULER_H+4);
        }
        if (a->ruler_out > a->ruler_in) {
            int xo = GUTTER_W + (int)((a->ruler_out/WB_SAMPLE_RATE - a->view_start/WB_SAMPLE_RATE)*(ARRANG_W/a->visible_secs));
            setc(a->ren, C_MUTE); SDL_RenderDrawLine(a->ren, xo, MAIN_Y, xo, MAIN_Y+RULER_H+3);
            region_add(70002, xo-4, MAIN_Y, 8, RULER_H+4);
        }
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
        /* R038: per-track Mute / Solo buttons in the gutter (conventional DAW track header)
         * G09: REC arm + delete + up/down reorder join the gutter row. */
        ui_button(a->ren, 6000+ti, GUTTER_W-90, y+4, 20, 14, "R", tr->rec_armed);
        ui_button(a->ren, 1000+ti, GUTTER_W-68, y+4, 20, 14, "M", tr->mute);
        ui_button(a->ren, 2000+ti, GUTTER_W-46, y+4, 20, 14, "S", tr->solo);
        ui_button(a->ren, 7000+ti, GUTTER_W-24, y+4, 20, 14, "x", 0);
        ui_button(a->ren, 7300+ti, GUTTER_W-46, y+20, 20, 14, "^", 0);
        ui_button(a->ren, 7400+ti, GUTTER_W-24, y+20, 20, 14, "v", 0);
        /* G09: the name itself is a region — double-click arms rename */
        {
            SDL_Rect nmr = { 4, y+4, GUTTER_W-96, 14 };
            region_add(500+ti, nmr.x, nmr.y, nmr.w, nmr.h);
        }
        if (a->rename_armed && a->rename_track == ti) {   /* rename cursor */
            SDL_Rect cur = { 6, y+16, (int)strlen(tr->name)*6 + 2, 2 };
            setc(a->ren, C_ACCENT); SDL_RenderFillRect(a->ren, &cur);
        }

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
                /* clip border — G13: tinted when the clip has a color slot */
                {
                    static const int cpal[8][3] = {
                        { 70, 70, 78},                    /* 0 default C_GRID-ish */
                        {220, 80, 80}, {235, 170, 60}, {110, 210, 90},
                        { 80, 190, 220}, {120, 120, 240}, {210, 100, 220},
                        {230, 230, 110} };
                    int cs = 0;
                    wb_clip_edit_table *cet = wb_engine_clip_edit(a->engine);
                    const wb_clip_edit *cce = cet ? wb_clip_edit_get(cet, ti, (int)c) : NULL;
                    if (cce) cs = cce->color & 7;
                    setc(a->ren, cpal[cs][0], cpal[cs][1], cpal[cs][2]);
                }
                SDL_RenderDrawRect(a->ren, &clipbox);
                int rx = clipbox.x + clipbox.w;
                /* R067: mark perf clips distinctly — they replay an event
                 * list, not media. Draw a dotted deck-grid pattern. */
                if (cl->type == 3 && cl->perfclip) {
                    setc(a->ren, C_ACCENT);
                    for (int gx = clipbox.x+4; gx < rx-4; gx += 12)
                        SDL_RenderDrawLine(a->ren, gx, clipbox.y+2,
                                           gx, clipbox.y+clipbox.h-2);
                    const char *lbl = "PERF";
                    wb_ui_draw_text(a->ren, clipbox.x+4, clipbox.y+2,
                                    lbl, 1, C_TEXT);
                }
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
                    /* G23/G64: fade handles as small draggable squares (when
                     * the lane is tall enough) with the curve drawn as a
                     * diagonal over the clip; right-click cycles the G64
                     * crossfade curve type (linear/equal-power/smoothstep). */
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
                    /* G23: solid square grips at both fade corners so the
                     * drag target is visible (Pro Tools/Ableton style) */
                    if (clipbox.h >= 12) {
                        SDL_Rect sq_in  = { clipbox.x - 2, clipbox.y - 2, 6, 6 };
                        SDL_Rect sq_out = { rx - 4,       clipbox.y - 2, 6, 6 };
                        SDL_RenderFillRect(a->ren, &sq_in);
                        SDL_RenderFillRect(a->ren, &sq_out);
                        /* G64: tiny curve glyph — L/E/S for the active type */
                        if (ce && ce->curve != 0) {
                            char cg[2] = { ce->curve == 1 ? 'E' : 'S', 0 };
                            wb_ui_draw_text(a->ren, clipbox.x + 8, clipbox.y + 2, cg, 1, C_FADE);
                        }
                    }
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
                 *    4 content-slide (top half), 5 loop-toggle,
                 *    6 clip-body MOVE (bottom half) — G14.
                 * decode: ti=(id-50000)/1000; rem%1000; c=rem/8; h=rem%8. */
                int base = 50000 + ti*1000 + (int)c*8;
                region_add(base+0, clipbox.x, clipbox.y, hw, clipbox.h);          /* LEFT trim  */
                region_add(base+1, clipbox.x+clipbox.w-hw, clipbox.y, hw, clipbox.h); /* RIGHT trim */
                region_add(base+2, clipbox.x, clipbox.y, hw+2, hw+2);              /* FADE_IN */
                region_add(base+3, clipbox.x+clipbox.w-hw-2, clipbox.y, hw+2, hw+2); /* FADE_OUT */
                region_add(base+4, clipbox.x+hw, clipbox.y, clipbox.w-2*hw, clipbox.h/2); /* CONTENT-SLIDE (top half) */
                region_add(base+5, rx-10, clipbox.y+clipbox.h-10, 10, 10);          /* LOOP toggle (bottom-right) */
                region_add(base+6, clipbox.x+hw, clipbox.y+clipbox.h/2,
                           clipbox.w-2*hw, clipbox.h/2);                            /* BODY MOVE (G14) */
                a->clip_handle_base[ti] = (clipbox.w > 16) ? base : -1;
                /* R022: clip (region) gain readout — pre-fader, like Pro Tools */
                if (cl->clip_gain > 1.001f || cl->clip_gain < 0.999f) {
                    char gb[16]; snprintf(gb, sizeof(gb), "g%.2f", cl->clip_gain);
                    wb_ui_draw_text(a->ren, clipbox.x+3, clipbox.y+3, gb, 1, C_ACCENT);
                }
                continue;
            }
            /* G14: MIDI clips get a clip box with trim/move handles, same
             * region encoding as audio clips (fades apply via the shared
             * wb_clip_edit_env render path). */
            if (cl->type == 0 && cl->note_count > 0) {
                int wx = arr_x(a, cl->start);
                int ww = (int)((cl->length/WB_SAMPLE_RATE)*arr_px_per_sec(a));
                if (ww < 4) ww = 4;
                int clip_h = lh - 4;
                if (clip_h < 6) clip_h = 6;
                SDL_Rect mbox = { wx, y+4 + cl->lane*lh, ww, clip_h };
                if (mbox.x < GUTTER_W) { int over = GUTTER_W-mbox.x; mbox.w -= over; mbox.x = GUTTER_W; }
                if (mbox.w > 4) {
                    setc(a->ren, C_FADE);
                    SDL_RenderDrawRect(a->ren, &mbox);
                    int mbase = 50000 + ti*1000 + (int)c*8;
                    int hw2 = 6;
                    region_add(mbase+0, mbox.x, mbox.y, hw2, mbox.h);                  /* LEFT trim */
                    region_add(mbase+1, mbox.x+mbox.w-hw2, mbox.y, hw2, mbox.h);       /* RIGHT trim */
                    region_add(mbase+6, mbox.x+hw2, mbox.y+1,
                               mbox.w-2*hw2, mbox.h-2);                                /* BODY MOVE (G14) */
                    a->clip_handle_base[ti] = mbase;
                }
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

        /* G30/G74: SEND A / SEND B rows — -/+ level buttons, % readout,
         * PRE toggle (lit = pre-fader), and a target label that cycles
         * through the session's bus tracks when clicked. Region ids:
         * 40000 + ti*8 + si*4 + {0 minus,1 plus,2 pre,3 target}. */
        for (int si = 0; si < 2; si++) {
            int base = 40000 + ti*8 + si*4;
            int ry = fy_bot + 46 + si*16;
            if (ry > WIN_H - STATUS_H - 20) break;
            char lbl[8]; snprintf(lbl, sizeof(lbl), "%c", 'A'+si);
            wb_ui_draw_text(a->ren, x+2, ry+3, lbl, 1, C_TEXT_DIM);
            ui_button(a->ren, base+0, x+12, ry, 14, 13, "-", 0);
            ui_button(a->ren, base+1, x+28, ry, 14, 13, "+", 0);
            char vbuf[8];
            snprintf(vbuf, sizeof(vbuf), "%d%%", (int)(tr->send_level[si]*100.0f + 0.5f));
            wb_ui_draw_text(a->ren, x+44, ry+3, vbuf, 1,
                            tr->send_level[si] > 0.0f ? C_ACCENT : C_TEXT_DIM);
            /* PRE/POST toggle: lit = PRE-fader */
            ui_button(a->ren, base+2, x+72, ry, 30, 13, "PRE", tr->send_pre[si]);
            /* target cycle button: shows the bus name (or "--") */
            const char *tname = "--";
            if (tr->send_target[si] >= 0
                && tr->send_target[si] < (int)a->session->track_count)
                tname = a->session->tracks[tr->send_target[si]].name;
            ui_button(a->ren, base+3, x+104, ry, sw-108 > 0 ? sw-108 : 10, 13,
                      tname, 0);
        }

        /* track name under fader */
        wb_ui_draw_text(a->ren, x+2, fy_top-16, tr->name, 1, C_TEXT);

        /* G31 FX rack: each insert row shows unit id + sidechain source;
         * left-click = cycle sidechain (G75), right-click = remove the FX.
         * A "+FX" button cycles a unit into the first empty slot. */
        if (ti == a->selected_track) {
            int iy = fy_bot + 80;   /* below the SEND A/B rows (G30) */
            int any = 0;
            for (int s = 0; s < WB_MAX_INSERT_SLOTS && s < 8; s++) {
                const char *id = tr->inserts[s].id;
                if (!id || !id[0]) continue;
                char chain[40];
                const char *src = "--";
                if (tr->sidechain[s] >= 0 &&
                    tr->sidechain[s] < (int)a->session->track_count)
                    src = a->session->tracks[tr->sidechain[s]].name;
                snprintf(chain, sizeof(chain), "%d:%s<-%s", s, id, src);
                ui_button(a->ren, 50000 + ti*8 + s, x+2, iy,
                          sw-18 > 10 ? sw-18 : 10, 12, chain,
                          tr->sidechain[s] >= 0);
                ui_button(a->ren, 60000 + ti*8 + s, x+sw-14, iy, 12, 12,
                          "X", 0);   /* G31: remove FX */
                iy += 14;
                any = 1;
                if (iy > WIN_H - 28) break;
            }
            if (!any)
                wb_ui_draw_text(a->ren, x+2, iy, "  (no inserts)", 1, C_TEXT_DIM);
            /* +FX: add the next unit from the rack palette to an empty slot */
            ui_button(a->ren, 61000 + ti*8, x+2, iy+2, sw-4 > 10 ? sw-4 : 10,
                      12, "+FX", 0);
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
        /* G32: live LUFS + true-peak readout under the master meter */
        {
            float st = 0.0f, tp = 0.0f;
            wb_engine_get_master_lufs(a->engine, &st, &tp);
            char lbuf[24];
            if (st < -69.0f || st == 0.0f)
                snprintf(lbuf, sizeof(lbuf), "LUFS --");
            else
                snprintf(lbuf, sizeof(lbuf), "LUFS %.1f", (double)st);
            wb_ui_draw_text(a->ren, mxk-6, fy_bot+20, lbuf, 1,
                            (st > -9.0f && st != 0.0f) ? C_MUTE : C_ACCENT);
            char tbuf[24];
            float tpdb = tp > 0.000001f ? 20*log10f(tp) : -120.0f;
            snprintf(tbuf, sizeof(tbuf), "TP %.1f", (double)tpdb);
            wb_ui_draw_text(a->ren, mxk-6, fy_bot+32, tbuf, 1,
                            tp > 0.99f ? C_MUTE : C_TEXT_DIM);
        }
    }
}

/* ---- R043 (G6): Fusion-style node-graph view -------------------------- */
/* Renders the self-contained comp_graph (Source -> Gain -> Composite) as
 * boxes + wires. The UI never touches node internals — only the opaque
 * accessors — so the compositor stays self-contained. */
static void draw_kf_editor(app *a);   /* G24: keyframe graph editor */
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
    draw_kf_editor(a);
}

/* ---- G24: keyframe graph editor for the Gain node's param -------------- */
/* A value/speed strip under the node graph. Click empty space = add key;
 * drag a key = retime/revalue; right-click a key = remove; keys interpolate
 * linearly (interp from the track). Region: kf editor canvas 620x120. */
#define KF_W 620
#define KF_H 120
static SDL_Rect kf_rect(app *a) {
    int ox = GUTTER_W + 16, oy = MAIN_Y + RULER_H + 40;
    return (SDL_Rect){ ox, oy + 300, KF_W, KF_H };
}
/* map editor-local coords <-> param space (t in [0,10]s, v in [0,2]) */
static void kf_to_screen(app *a, double t, float v, int *sx, int *sy) {
    SDL_Rect r = kf_rect(a);
    *sx = r.x + (int)(t / 10.0 * (r.w - 8)) + 4;
    *sy = r.y + r.h - 6 - (int)(v / 2.0f * (r.h - 12));
}
static void screen_to_kf(app *a, int sx, int sy, double *t, float *v) {
    SDL_Rect r = kf_rect(a);
    *t = (double)(sx - r.x - 4) / (r.w - 8) * 10.0;
    if (*t < 0) *t = 0; if (*t > 10) *t = 10;
    *v = (float)(r.y + r.h - 6 - sy) / (r.h - 12) * 2.0f;
    if (*v < 0) *v = 0; if (*v > 2) *v = 2;
}
static void draw_kf_editor(app *a) {
    if (!a->kf_track) return;
    SDL_Rect r = kf_rect(a);
    setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &r);
    setc(a->ren, C_TEXT_DIM); SDL_RenderDrawRect(a->ren, &r);
    wb_ui_draw_text(a->ren, r.x+4, r.y+2, "GAIN KEYFRAMES  L-add  drag-move  R-del", 1, C_TEXT_DIM);
    /* curve */
    setc(a->ren, C_FADE);
    int px = -1, py = -1;
    for (int i = 0; i <= r.w - 8; i++) {
        double t = (double)i / (r.w - 8) * 10.0;
        float v = wb_param_track_value_at(a->kf_track, t);
        int sx, sy; kf_to_screen(a, t, v, &sx, &sy);
        if (px >= 0) SDL_RenderDrawLine(a->ren, px, py, sx, sy);
        px = sx; py = sy;
    }
    /* keys */
    int n = wb_param_track_count(a->kf_track);
    for (int i = 0; i < n; i++) {
        wb_keyframe k;
        if (wb_param_track_key_index(a->kf_track, i, &k) != 0) continue;
        int sx, sy; kf_to_screen(a, k.t, k.value, &sx, &sy);
        SDL_Rect kb = { sx-3, sy-3, 7, 7 };
        setc(a->ren, i == a->kf_drag ? C_ACCENT : C_TEXT);
        SDL_RenderFillRect(a->ren, &kb);
    }
}
/* returns 1 if the click was inside the kf editor (and handled) */
static int kf_click(app *a, int x, int y, int button) {
    if (!a->kf_track) return 0;
    SDL_Rect r = kf_rect(a);
    if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) return 0;
    double t; float v; screen_to_kf(a, x, y, &t, &v);
    if (button == SDL_BUTTON_RIGHT) {
        /* remove nearest key within tolerance */
        for (int i = 0; i < wb_param_track_count(a->kf_track); i++) {
            wb_keyframe k;
            if (wb_param_track_key_index(a->kf_track, i, &k) != 0) continue;
            int sx, sy; kf_to_screen(a, k.t, k.value, &sx, &sy);
            if (abs(sx-x) < 6 && abs(sy-y) < 6) {
                wb_param_track_remove(a->kf_track, k.t);
                printf("kf: removed key @ %.2fs\n", k.t);
                return 1;
            }
        }
        return 1;
    }
    /* left-click on an existing key starts a drag; else add a new key */
    for (int i = 0; i < wb_param_track_count(a->kf_track); i++) {
        wb_keyframe k;
        if (wb_param_track_key_index(a->kf_track, i, &k) != 0) continue;
        int sx, sy; kf_to_screen(a, k.t, k.value, &sx, &sy);
        if (abs(sx-x) < 6 && abs(sy-y) < 6) {
            a->kf_drag = i;
            a->kf_drag_t0 = k.t;
            return 1;
        }
    }
    wb_param_track_set(a->kf_track, t, v, WB_KF_LINEAR);
    printf("kf: added key @ %.2fs = %.2f\n", t, (double)v);
    return 1;
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
/* R065/R068: the agent bridge renders through THIS performance instance.
 * wb_agent_perf_target = render path; wb_agent_perf_live = snapshot path.
 * Both resolve to the DAW's active perf (NULL when no DAW is attached). */
void *wb_agent_perf_target(void) {
    extern app *g_app_for_perf;
    return g_app_for_perf ? g_app_for_perf->perf : NULL;
}
wb_perf *wb_agent_perf_live(void) {
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
        /* R069: live deck thumbnail — render this single deck's state
         * into a cell-sized RGBA buffer and draw it, rather than a flat
         * color. The perf's render cache handles invalidation. Idle
         * (unfired) decks fall back to a dim lane color. */
        int fired = wb_perf_deck_fired(a->perf, d);
        if (fired) {
            static uint8_t thumb[160*100*4];
            /* render the deck at the current transport position */
            wb_perf_set_clock(a->perf, a->t.song_pos / WB_SAMPLE_RATE);
            wb_perf_seek(a->perf, a->t.song_pos / WB_SAMPLE_RATE);
            wb_perf_render_frame(a->perf, thumb);
            /* blit the thumbnail into the cell, nearest-neighbor */
            for (int yy2 = 0; yy2 < ph; yy2++)
                for (int xx = 0; xx < pw; xx++) {
                    int sx = xx * 160 / pw;
                    int sy = yy2 * 100 / ph;
                    int sp = (sy*160 + sx)*4;
                    if (thumb[sp+3] > 128) {
                        setc(a->ren, thumb[sp], thumb[sp+1], thumb[sp+2]);
                        SDL_RenderDrawPoint(a->ren, cx+xx, cy+yy2);
                    }
                }
        } else {
            /* idle deck: dim lane */
            setc(a->ren, ((d % 2) == 0) ? 40 : 24,
                            ((d % 2) == 0) ? 30 : 48,
                            ((d % 2) == 0) ? 16 : 64);
            SDL_RenderFillRect(a->ren, &cell);
        }
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
static int video_import(app *a, const char *path);   /* Wave1 G02 (lane A) */
static void draw_action_bar(app *a);
static void draw_status(app *a);
static void draw_overview(app *a);
static void handle_action(app *a, int act);
static int  video_import(app *a, const char *path);   /* Wave1 G01: fwd decl */
static void draw_browser(app *a);                     /* Wave1 G01 */

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
    if (a->browser_open) draw_browser(a);   /* Wave1 G01: modal on top */
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

    /* G89: SWING -/+ + % readout, right of the tabs over the mixer column.
     * swing is a session-level fraction of a 16th (0..0.6); MPC-spec
     * display maps delay fraction -> % swung (50% straight .. 75%+).
     * G80/G81/G87/G88: scale-lock, chord stamp and scale cycle buttons share
     * the same mixer-column strip. */
    {
        double sw = a->session ? a->session->swing : 0.0;
        if (sw < 0) sw = 0;
        if (sw > 0.6) sw = 0.6;
        int sx = WIN_W - MIXER_W + 8;
        int sbx = 22, sby = by, sbh = bh;
        ui_button(a->ren, BTN_SWING_M, sx,        sby, sbx, sbh, "-", 0);
        ui_button(a->ren, BTN_SWING_P, sx+26,     sby, sbx, sbh, "+", 0);
        char sbuf[32];
        snprintf(sbuf, sizeof(sbuf), "SWING %d%%", (int)((0.5 + sw) * 100.0 + 0.5));
        wb_ui_draw_text(a->ren, sx+54, sby + sbh/2 - 4, sbuf, 1,
                        sw > 0.0 ? C_ACCENT : C_TEXT_DIM);

        /* G80: SCALE ROOT + TYPE cycle (small, left-aligned after swing) */
        int gx = sx + 110;
        char rbuf[16];
        snprintf(rbuf, sizeof(rbuf), "ROOT %s", scale_root_name(a->scale_root));
        ui_button(a->ren, BTN_SCALE_ROOT, gx,        sby, 62, sbh, rbuf,        0);
        ui_button(a->ren, BTN_SCALE_TYPE, gx+66,     sby, 80, sbh,
                  scale_name(a->scale_root, a->scale_type), a->scale_lock);
        /* G80: LOCK toggle + G81: CHORD cycle */
        ui_button(a->ren, BTN_LOCK,     gx+150,    sby, 70, sbh,
                  a->scale_lock ? "LOCK" : "LOCK", a->scale_lock);
        const char *cn = a->chord_mode ? (a->chord_mode==1?"TRIAD":a->chord_mode==2?"7TH":"9TH") : "CHORD";
        ui_button(a->ren, BTN_CHORD,    gx+224,    sby, 62, sbh, cn, a->chord_mode);
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
        "VIEW: %s    TRACK: %s (%d/%d)    %s    BPM %.1f    %g Hz    |    %s",
        vname, tkname, tk+1, tcount, state, a->t.bpm, a->t.sample_rate,
        a->last_status[0] ? a->last_status
            : "click tabs+buttons • SPACE play/stop • R rewind • M marker/roll , . ripple-trim");
    wb_ui_draw_text(a->ren, 10, y+5, line, 1,
                    a->last_status[0] ? 96 : 165, a->last_status[0] ? 220 : 165, 255);
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
        ui_button(a->ren, BTN_ACT1, bx+bw+6,   by, bw, bh, "CAPTURE", 0);   /* G93 */
    } else if (tab == 2) {  /* STEP */
        ui_button(a->ren, BTN_ACT0, bx,        by, bw, bh, "CLEAR", 0);
        ui_button(a->ren, BTN_ACT1, bx+bw+6,  by, bw, bh, "COMMIT", 0);
        ui_button(a->ren, BTN_ACT2, bx+2*(bw+6), by, bw, bh, "CAPTURE", 0); /* G93 */
        ui_button(a->ren, BTN_ACT3, bx+3*(bw+6), by, bw, bh, "FILL", 0);    /* G91: shift=4th alt=rand */
        ui_button(a->ren, 62000, bx+4*(bw+6), by, bw, bh, "RETRIG", 0);     /* G92 */
    } else if (tab == 3) {  /* SESSION */
        ui_button(a->ren, BTN_ACT0, bx,        by, bw, bh, "STOP ALL", 0);
        ui_button(a->ren, BTN_ACT1, bx+bw+6,   by, bw, bh,
                  "REC ARR", a->launchrec_armed);                            /* G94 */
        ui_button(a->ren, BTN_ACT2, bx+2*(bw+6), by, bw, bh,
                  "SONG", a->song_len > 0);                                  /* G90 */
        {   /* G95: follow-action cycle for the current chain position */
            static const char *fan[] = { "NEXT", "JUMP", "OTHER", "FILL", "LEGATO" };
            char flbl[24];
            snprintf(flbl, sizeof(flbl), "%.6s %d%%",
                     fan[a->fa_action[a->song_pos >= 0 ? a->song_pos : 0] & 7],
                     a->fa_chance[a->song_pos >= 0 ? a->song_pos : 0]);
            ui_button(a->ren, 63000, bx+3*(bw+6), by, bw, bh, flbl,
                      a->fa_action[0] != 0);                                 /* G95 */
        }
    } else {  /* video tabs 4..7 */
        ui_button(a->ren, BTN_ACT0, bx,        by, bw, bh, "IMPORT", 0);
        ui_button(a->ren, BTN_ACT1, bx+bw+6,  by, bw, bh, "CAPTIONS", 0);
        ui_button(a->ren, BTN_ACT2, bx+2*(bw+6), by, bw, bh, "EXPORT", 0);
        if (tab == 5)   /* EDIT: freeze the live perf into the timeline */
            ui_button(a->ren, BTN_ACT3, bx+3*(bw+6), by, bw, bh, "FREEZE", a->perf_recording);
        if (tab == 6) { /* CAPTIONS: delivery master (normalize + chapters) */
            ui_button(a->ren, BTN_ACT3, bx+3*(bw+6), by, bw/2-3, bh, "DELIVER", 0);
            ui_button(a->ren, BTN_WS4+100, bx+3*(bw+6)+bw/2+3, by, bw/2-3, bh, "CHAP", 0);
            /* G46: SRT roundtrip buttons */
            int sx = bx+4*(bw+6);
            ui_button(a->ren, 9101, sx, by, bw/2-3, bh, "SRT OUT",
                      a->vid_captions_ready);
            ui_button(a->ren, 9102, sx+(bw/2+3), by, bw/2-3, bh, "SRT IN", 0);
            /* G44: title tool — TSET arms text entry, TBURN bakes it */
            ui_button(a->ren, 9103, sx+2*(bw/2+6), by, bw/2-3, bh,
                      a->title_entry ? "TEXT..." : "TITLE", a->title_entry);
            ui_button(a->ren, 9104, sx+2*(bw/2+6)+bw/2+3, by, bw/2-3, bh,
                      "TBURN", 0);
        }
        if (tab == 7) { /* EXPORT: codec choice + perf passthrough */
            ui_button(a->ren, BTN_ACT3, bx+3*(bw+6), by, bw, bh,
                      a->export_codec_h264 ? "H264" : "PRORES", 0);
            /* G39/G40: range + resolution row */
            int ex = bx+4*(bw+6);
            int sw = 64;
            ui_button(a->ren, 9001, ex, by, sw, bh,
                      a->export_range_mode ? "IN-OUT" : "WHOLE",
                      a->export_range_mode);
            ui_button(a->ren, 9002, ex+(sw+4),    by, sw, bh, "480p", a->export_res_h==480);
            ui_button(a->ren, 9003, ex+2*(sw+4),  by, sw, bh, "720p", a->export_res_h==720);
            ui_button(a->ren, 9004, ex+3*(sw+4),  by, sw, bh, "1080p", a->export_res_h==0);
            if (wb_export_job_running(&a->ejob))
                ui_button(a->ren, 9005, ex+4*(sw+4), by, sw, bh, "CANCEL", 1);
            /* G52: delivery preset cycle + DELIVER (profile-normalized master) */
            {
                ui_button(a->ren, 9100, ex+5*(sw+4), by, sw+20, bh, g_delivery_cycle[a->delivery_profile_idx], 1);
                int dx = ex + 5*(sw+4) + sw + 24;
                ui_button(a->ren, 9101, dx, by, 76, bh, "DELIVER", 0);
                ui_button(a->ren, 9102, dx+80, by, 70, bh, "STEMS", 0);   /* G41 */
            }
        }
        if (tab == 4)   /* Wave1 G01: open the in-GUI media browser */
            ui_button(a->ren, BTN_BROWSE, bx+3*(bw+6), by, bw, bh, "BROWSE", a->browser_open);
    }
}

/* ---- Wave1 G01/G02: in-GUI media browser + audio import ------------------ */

/* G08 helper: snapshot the session BEFORE a mutation so UNDO can restore it. */
static void daw_checkpoint(app *a) {
    if (a->undo && a->session) wb_undo_checkpoint(a->undo, a->session);
}

/* Enumerate media files from the standard user folders into the browser list. */
static void browser_scan(app *a) {
    static const char *dirs[] = {
        "/Users/waefrebeorn/Movies", "/Users/waefrebeorn/Music",
        "/Users/waefrebeorn/Desktop", "/Users/waefrebeorn/Documents",
    };
    a->browser_count = 0;
    for (size_t d = 0; d < sizeof(dirs)/sizeof(dirs[0]); d++) {
        int n = wb_import_scan_dir(dirs[d],
                &a->browser_paths[a->browser_count],
                128 - a->browser_count);
        if (n > 0) a->browser_count += n;
        if (a->browser_count >= 128) { a->browser_count = 128; break; }
    }
    a->browser_scroll = 0;
}

/* Import one browsed/picked file: video via the existing proxy path,
 * everything else through the G02 audio-import path at the playhead. */
static void browser_import(app *a, const char *path) {
    if (!path || !path[0]) return;
    size_t pl = strlen(path);
    int is_video = (pl > 4 && (!strcasecmp(path+pl-4, ".mp4") ||
                               !strcasecmp(path+pl-4, ".mov")));
    /* G46: SRT import — load captions into the video editor context */
    if (pl > 4 && !strcasecmp(path+pl-4, ".srt")) {
        snprintf(a->vid_srt, sizeof(a->vid_srt), "%s", path);
        if (a->vid_tr) wb_transcript_free(a->vid_tr);
        a->vid_tr = wb_transcript_from_srt(path);
        a->tr_sel0 = a->tr_sel1 = 0;
        a->vid_captions_ready = a->vid_tr != NULL;
        snprintf(a->last_status, sizeof(a->last_status),
                 "SRT LOADED: %d lines", a->vid_tr ? wb_transcript_count(a->vid_tr) : 0);
        printf("srt-import: %s (%d lines)\n", path,
               a->vid_tr ? wb_transcript_count(a->vid_tr) : 0);
        return;
    }
    daw_checkpoint(a);
    if (is_video) {
        if (video_import(a, path) == 0)
            snprintf(a->last_status, sizeof(a->last_status), "IMPORTED VIDEO %.32s", path);
        return;
    }
    if (!a->session) { a->session = wb_session_create(); wb_engine_set_session(a->engine, a->session); }
    double pos = a->t.song_pos / WB_SAMPLE_RATE;
    char err[128] = {0};
    if (wb_import_audio_file(a->session, path, pos, err, sizeof(err)) == 0) {
        wb_engine_set_session(a->engine, a->session);
        snprintf(a->last_status, sizeof(a->last_status),
                 "IMPORTED AUDIO -> track %d @ %.1fs", wb_import_last_track(), pos);
        printf("import: %s -> track %d @ %.2fs\n", path, wb_import_last_track(), pos);
    } else {
        snprintf(a->last_status, sizeof(a->last_status), "IMPORT FAILED: %s", err);
        fprintf(stderr, "import: %s\n", err);
    }
}

/* Modal-ish overlay list of scanned media files; click row = import. */
static void draw_browser(app *a) {
    /* dim backdrop */
    SDL_Rect full = { 0, 0, WIN_W, WIN_H };
    setc(a->ren, 10, 10, 12); SDL_RenderFillRect(a->ren, &full);
    int bw2 = 720, bh2 = 520;
    int bx = (WIN_W - bw2) / 2, by = (WIN_H - bh2) / 2;
    SDL_Rect panel = { bx, by, bw2, bh2 };
    setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &panel);
    setc(a->ren, C_ACCENT);
    SDL_Rect title = { bx, by, bw2, 26 };
    SDL_RenderFillRect(a->ren, &title);
    setc(a->ren, C_BG);
    wb_ui_draw_text(a->ren, bx + 8, by + 6, "MEDIA BROWSER — click a file to import", 1, C_BG);

    const int row_h = 24, top = by + 34, visible = (bh2 - 80) / row_h;
    if (a->browser_count == 0) {
        setc(a->ren, C_TEXT_DIM);
        wb_ui_draw_text(a->ren, bx + 12, top + 8,
                        "No media found in ~/Movies ~/Music ~/Desktop ~/Documents", 1, C_TEXT_DIM);
    }
    for (int i = 0; i < visible; i++) {
        int idx = a->browser_scroll + i;
        if (idx >= a->browser_count) break;
        ui_button(a->ren, BTN_BROWSER_ROW0 + idx, bx + 10, top + i * row_h,
                  bw2 - 20, row_h - 3, a->browser_paths[idx], 0);
    }
    char cnt[64];
    snprintf(cnt, sizeof(cnt), "%d files — ESC closes, wheel scrolls", a->browser_count);
    setc(a->ren, C_TEXT_DIM);
    wb_ui_draw_text(a->ren, bx + 10, by + bh2 - 30, cnt, 1, C_TEXT_DIM);
    ui_button(a->ren, BTN_BROWSER_CANCEL, bx + bw2 - 90, by + bh2 - 34, 80, 26, "CANCEL", 0);
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

/* ---- Wave2 lane B: G15 TRIM MODE + G65 precision two-up display -------- */

#define WB_TRIM_FRAME (1.0 / 25.0)   /* frame-wise nudge step (1/25 s) */

static void trim_mode_exit(app *a) {
    a->trim_mode = 0;
    for (int i = 0; i < 2; i++) {
        if (a->trim_tex[i]) { SDL_DestroyTexture(a->trim_tex[i]); a->trim_tex[i] = NULL; }
        a->trim_tex_key[i][0] = 0;
    }
}

/* Enter TRIM MODE on the edit point of clip `ci` nearest the playhead. */
static void trim_mode_enter(app *a, int ci) {
    if (!a->session || !a->vid_has_clip) return;
    if (ci < 0 || (uint32_t)ci >= a->session->tracks[a->vid_track].clip_count)
        ci = a->vid_clip;
    if (ci < 0) return;
    wb_clip *cl = &a->session->tracks[a->vid_track].clips[ci];
    if (cl->type != 2 || !cl->video) return;
    double ph = a->t.song_pos / WB_SAMPLE_RATE;
    int edge = 1;
    wb_precision_nearest_edge(a->session, a->vid_track, ci, ph, &edge);
    trim_mode_exit(a);
    a->trim_clip = ci;
    a->trim_edge = edge;
    a->trim_mode = 1;
    snprintf(a->last_status, sizeof(a->last_status),
             "TRIM MODE: %s of clip %d (arrows/,/. nudge, JKL shuttle, ESC exits)",
             edge == 0 ? "IN" : "OUT", ci);
    printf("trim: ENTER on %s of clip %d\n", edge == 0 ? "in" : "out", ci);
}

static void trim_nudge(app *a, double delta) {
    if (!a->session || !a->vid_has_clip || !a->trim_mode) return;
    int rc = wb_session_nudge_edit_point(a->session, a->vid_track,
                                         a->trim_clip, a->trim_edge, delta);
    if (rc == 0)
        snprintf(a->last_status, sizeof(a->last_status), "TRIM %+d frames",
                 (int)(delta / WB_TRIM_FRAME + (delta >= 0 ? 0.5 : -0.5)));
    printf("trim: nudge %+.3fs rc=%d\n", delta, rc);
}

/* G65: grab one frame near the cut into a cached texture. side 0 = outgoing
 * (last frame before cut), side 1 = incoming (first frame after cut). */
static SDL_Texture *trim_frame_tex(app *a, int side, double src_time) {
    wb_clip *cl = &a->session->tracks[a->vid_track].clips[a->trim_clip];
    wb_clip *other = &a->session->tracks[a->vid_track].clips[
        a->trim_clip + (side == 0 ? 0 : 1)];
    wb_video_clip *vc = side == 0 ? cl->video : other->video;
    if (!vc || !vc->proxy_path[0]) vc = cl->video;
    char key[640];
    snprintf(key, sizeof(key), "%s@%.3f", vc->proxy_path, src_time);
    if (a->trim_tex[side] && strcmp(key, a->trim_tex_key[side]) == 0)
        return a->trim_tex[side];
    if (a->trim_tex[side]) { SDL_DestroyTexture(a->trim_tex[side]); a->trim_tex[side] = NULL; }
    wb_video_decoder *vd = wb_video_decoder_open(vc->proxy_path);
    if (!vd) return NULL;
    uint8_t *rgba = malloc(854 * 480 * 4);
    int ow = 0, oh = 0;
    if (rgba && wb_video_decoder_seek(vd, src_time) == 0 &&
        wb_video_decoder_decode_frame(vd, rgba, &ow, &oh) == 0)
        a->trim_tex[side] = wb_video_frame_to_texture(a->ren, rgba, ow, oh);
    free(rgba);
    wb_video_decoder_close(vd);
    snprintf(a->trim_tex_key[side], sizeof(a->trim_tex_key[side]), "%s", key);
    return a->trim_tex[side];
}

/* G65: two-up display — outgoing last frames | incoming first frames with a
 * center divider at the edit point. Falls back to note/waveform context bars
 * for MIDI/audio clips or when no decoder is available. */
static void draw_trim_twoup(app *a, int px, int py, int pw) {
    if (!a->session || !a->vid_has_clip || !a->trim_mode) return;
    wb_track *tr = &a->session->tracks[a->vid_track];
    if ((uint32_t)a->trim_clip >= tr->clip_count) return;
    wb_clip *l = &tr->clips[a->trim_clip];
    wb_clip *r = a->trim_clip + 1 < (int)tr->clip_count
               ? &tr->clips[a->trim_clip + 1] : NULL;

    /* G15 badge */
    SDL_Rect badge = { px + 6, py, 110, 16 };
    setc(a->ren, 200, 60, 60);
    SDL_RenderFillRect(a->ren, &badge);
    setc(a->ren, 255, 255, 255);
    wb_ui_draw_text(a->ren, px + 12, py + 2,
                    a->trim_edge == 0 ? "TRIM: IN PT" : "TRIM: OUT PT",
                    1, 255, 255, 255);

    int ty = py + 22, th = 90, half = (pw - 16) / 2;
    double cut_src_l = l->video->start_in_source
                     + (l->length > 0 ? l->length : l->video->duration)
                     - WB_TRIM_FRAME / 2.0;
    if (cut_src_l < 0) cut_src_l = 0;
    SDL_Texture *tl_tex = trim_frame_tex(a, 0, cut_src_l);

    int lx = px + 6, rx = px + 6 + half + 4;
    SDL_Rect lrect = { lx, ty, half, th }, rrect = { rx, ty, half, th };
    setc(a->ren, C_PANEL2); SDL_RenderFillRect(a->ren, &lrect);
    SDL_RenderFillRect(a->ren, &rrect);
    if (tl_tex) wb_video_blit_scaled(a->ren, tl_tex, &lrect);
    if (r && r->type == 2 && r->video) {
        double cut_src_r = r->video->start_in_source < 0 ? 0
                         : r->video->start_in_source + WB_TRIM_FRAME / 2.0;
        SDL_Texture *tr_tex = trim_frame_tex(a, 1, cut_src_r);
        if (tr_tex) wb_video_blit_scaled(a->ren, tr_tex, &rrect);
    } else if (r && r->type == 0 && r->notes && r->note_count) {
        /* MIDI context: first notes of the incoming clip as pitch bars */
        setc(a->ren, 90, 160, 240);
        for (uint32_t n = 0; n < r->note_count && n < 24; n++) {
            int bh = 4 + (r->notes[n].pitch % 24) * 3;
            SDL_Rect bar = { rx + 4 + (int)n * ((half - 8) / 24), ty + th - bh - 2,
                             (half - 8) / 24 - 1, bh };
            SDL_RenderFillRect(a->ren, &bar);
        }
    } else if (r && r->type == 1 && r->audio_data && r->audio_frames) {
        /* waveform context: coarse envelope of the incoming audio head */
        setc(a->ren, 90, 220, 120);
        int bars = half - 8;
        for (int bxi = 0; bxi < bars; bxi += 3) {
            size_t idx = (size_t)((double)bxi / bars * r->audio_frames)
                       * r->audio_channels;
            float v = fabsf(r->audio_data[idx]);
            int bh = (int)(v * (th - 6));
            if (bh > th - 6) bh = th - 6;
            SDL_Rect bar = { rx + 4 + bxi, ty + th - bh - 2, 2, bh };
            SDL_RenderFillRect(a->ren, &bar);
        }
    } else if (r) {
        wb_ui_draw_text(a->ren, rx + 6, ty + th / 2 - 6, "(end of track)",
                        1, C_TEXT_DIM);
    }

    /* center divider AT the edit point */
    setc(a->ren, 255, 210, 70);
    for (int i = 0; i < 2; i++)
        SDL_RenderDrawLine(a->ren, rx - 3 + i, ty - 2, rx - 3 + i, ty + th + 2);

    setc(a->ren, C_TEXT_DIM);
    wb_ui_draw_text(a->ren, lx + 4, ty + th + 2, "OUTGOING (last frame)", 1, C_TEXT_DIM);
    wb_ui_draw_text(a->ren, rx + 4, ty + th + 2, "INCOMING (first frame)", 1, C_TEXT_DIM);
    char tc[64];
    snprintf(tc, sizeof(tc), "cut @ %.2fs  (%s)", l->start + l->length,
             a->trim_edge == 0 ? "IN" : "OUT");
    wb_ui_draw_text(a->ren, lx + 4, ty + th + 14, tc, 1, C_ACCENT);
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
        /* Wave2 G15/G65: trim-mode badge + precision two-up preview */
        if (a->trim_mode && a->vid_has_clip) {
            draw_trim_twoup(a, px, yy + 2, pw);
            yy += 140;
            wb_ui_draw_text(a->ren, px + 6, yy,
                            "< > or , . nudge 1 frame | JKL shuttle", 1, C_ACCENT); yy += 14;
        }
        wb_ui_draw_text(a->ren, px + 6, yy, "T trim mode  R razor  O drop mode:", 1, C_ACCENT); yy += 14;
        wb_ui_draw_text(a->ren, px + 6, yy,
                        a->drop_insert ? "  INSERT (later clips shift)" : "  OVERWRITE", 1, C_ACCENT); yy += 14;
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
        /* G39/G40: reflect the actual settings */
        snprintf(buf, sizeof(buf), "Range: %s",
                 a->export_range_mode ? "IN -> playhead" : "WHOLE");
        wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 14;
        snprintf(buf, sizeof(buf), "Resolution: %s",
                 a->export_res_h == 480 ? "854x480" :
                 a->export_res_h == 720 ? "1280x720" : "1920x1080");
        wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 14;
        /* G52: active delivery preset */
        {
            const wb_delivery_profile *dp = daw_delivery_profile(a->delivery_profile_idx);
            snprintf(buf, sizeof(buf), "Profile: %s (%.1f LUFS)",
                     g_delivery_cycle[a->delivery_profile_idx],
                     dp ? dp->lufs : -14.0);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_ACCENT); yy += 14;
        }
        /* G38: background render queue — progress bar polled each frame */
        if (wb_export_job_running(&a->ejob)) {
            int pct = (int)(a->ejob.progress * 100.0 + 0.5);
            SDL_Rect barbg = { px + 6, yy, pw - 12, 14 };
            setc(a->ren, C_PANEL2); SDL_RenderFillRect(a->ren, &barbg);
            int fw = (int)((pw - 12) * a->ejob.progress);
            if (fw < 2) fw = 2;
            SDL_Rect fill = { px + 6, yy, fw, 14 };
            setc(a->ren, C_ACCENT); SDL_RenderFillRect(a->ren, &fill);
            char pbuf[64];
            snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
            wb_ui_draw_text(a->ren, px + 10, yy + 3, pbuf, 1, 255,255,255);
            yy += 20;
            wb_ui_draw_text(a->ren, px + 6, yy, "RENDERING IN BACKGROUND...", 1, C_ACCENT);
            yy += 14;
        } else if (a->ejob.done) {
            snprintf(buf, sizeof(buf), a->ejob.cancelled ? "Render CANCELLED" :
                     a->ejob.rc == 0 ? "Render COMPLETE" : "Render FAILED");
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1,
                            a->ejob.rc == 0 && !a->ejob.cancelled ? C_TEXT : 235,110,110);
            yy += 16;
        }
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
    /* G66: INSERT drop — ripple existing video clips right to make room */
    if (a->drop_insert && a->vid_dur > 0) {
        int shifted = wb_session_drop_place(a->session, vt, 0.0,
                                            a->vid_dur, WB_DROP_INSERT);
        if (shifted > 0) printf("drop: INSERT shifted %d clip(s) on import\n", shifted);
    }
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
    a->export_codec_h264 = 1; a->last_status[0] = 0;
    a->caplog = wb_caplog_create();          /* G93 */
    a->capture_window = 8.0;
    a->lrec = wb_launchrec_create();         /* G94 */
    a->launchrec_armed = 0;
    memset(&a->ejob, 0, sizeof a->ejob);     /* G38 */
    a->export_range_mode = 0;                /* G39: WHOLE by default */
    a->export_res_h = 0;                     /* G40: native by default */
    a->delivery_profile_idx = 0;             /* G52: YOUTUBE default */
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
    } else if (st == 0xB0) {
        /* G35: MIDI learn — CC maps to a bound target */
        int cc = ev.data1, val = ev.data2;
        if (a->midi_learn_armed) {
            /* bind: reuse an existing entry for this CC or append */
            int slot = -1;
            for (int i = 0; i < a->midi_map_n; i++)
                if (a->midi_map[i].cc == cc) { slot = i; break; }
            if (slot < 0 && a->midi_map_n < 16)
                slot = a->midi_map_n++;
            if (slot >= 0) {
                a->midi_map[slot].cc = cc;
                a->midi_map[slot].target = a->midi_learn_target;
                snprintf(a->last_status, sizeof(a->last_status),
                         "LEARNED CC %d -> target %d", cc,
                         a->midi_learn_target);
                printf("midilearn: CC %d -> %d\n", cc, a->midi_learn_target);
            }
            a->midi_learn_armed = 0;
            return;
        }
        for (int i = 0; i < a->midi_map_n; i++) {
            if (a->midi_map[i].cc != cc) continue;
            float f = val / 127.0f;
            switch (a->midi_map[i].target) {
            case 0:  /* master volume */
                wb_engine_set_master_volume(a->engine, f);
                break;
            case 1:  /* selected track volume */
                if (a->selected_track >= 0)
                    wb_engine_set_track_volume(a->engine, a->selected_track, f);
                break;
            case 2:  /* tempo 60..180 */
                wb_engine_set_bpm(a->engine, 60.0 + f * 120.0);
                break;
            case 3:  /* selected track insert slot 1, param 0 */
                if (a->selected_track >= 0)
                    wb_engine_set_insert_param(a->engine, a->selected_track,
                                               1, 0, f);
                break;
            }
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
static void recent_add(const char *path);   /* G11 fwd decl */
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
    recent_add(path);   /* G11 */
    printf("project: loaded %s (%u tracks)\n", path, s->track_count);
    return 0;
}

/* ---- G11: recent files + templates -------------------------------------- */
#define WB_RECENTS_FILE "/tmp/bigmac_recent.wbuslist"
#define WB_RECENTS_MAX  8
static void recent_add(const char *path) {
    if (!path || !path[0]) return;
    /* load existing list, drop dupes, prepend, cap at N, rewrite */
    char lines[WB_RECENTS_MAX][512];
    int n = 0;
    FILE *fi = fopen(WB_RECENTS_FILE, "r");
    if (fi) {
        char ln[512];
        while (n < WB_RECENTS_MAX && fgets(ln, sizeof(ln), fi)) {
            size_t l = strlen(ln);
            while (l && (ln[l-1]=='\n'||ln[l-1]=='\r')) ln[--l] = 0;
            if (ln[0] && strcmp(ln, path) != 0)
                snprintf(lines[n++], sizeof(lines[0]), "%s", ln);
        }
        fclose(fi);
    }
    FILE *fo = fopen(WB_RECENTS_FILE ".tmp", "w");
    if (!fo) return;
    fprintf(fo, "%s\n", path);
    for (int i = 0; i < n; i++) fprintf(fo, "%s\n", lines[i]);
    fclose(fo);
    rename(WB_RECENTS_FILE ".tmp", WB_RECENTS_FILE);
}
static int recent_list(char out[][512], int max) {
    FILE *fi = fopen(WB_RECENTS_FILE, "r");
    if (!fi) return 0;
    int n = 0;
    char ln[512];
    while (n < max && fgets(ln, sizeof(ln), fi)) {
        size_t l = strlen(ln);
        while (l && (ln[l-1]=='\n'||ln[l-1]=='\r')) ln[--l] = 0;
        if (ln[0]) snprintf(out[n++], 512, "%s", ln);
    }
    fclose(fi);
    return n;
}
/* G11: save the current session as a template (empty notes/clips, same
 * track layout + inserts). Templates live in /tmp/bigmac_templates/. */
static void save_template(app *a) {
    if (!a->session) return;
    mkdir("/tmp/bigmac_templates", 0755);
    char p[512];
    snprintf(p, sizeof(p), "/tmp/bigmac_templates/tmpl_%lld.wbus",
             (long long)time(NULL));
    /* strip content but keep the rack: remove all clips + notes */
    wb_session *t = a->session;
    for (uint32_t i = 0; i < t->track_count; i++) {
        wb_track *tr = &t->tracks[i];
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            free(tr->clips[c].notes); tr->clips[c].notes = NULL;
            tr->clips[c].note_count = 0;
        }
    }
    if (wb_session_save(t, p) == 0) {
        snprintf(a->last_status, sizeof(a->last_status),
                 "TEMPLATE SAVED %.44s", p);
        printf("template: %s\n", p);
    }
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
    recent_add(dst);   /* G11 */
    /* G59: versioned backup — a timestamped snapshot beside the project
     * (Live Save convention). Keeps the last 10; failures are non-fatal. */
    {
        char bdir[600];
        snprintf(bdir, sizeof(bdir), "%.450s.backups", dst);
        mkdir(bdir, 0755);
        char bpath[768];
        snprintf(bpath, sizeof(bpath), "%s/%lld.wbus", bdir, (long long)time(NULL));
        if (wb_session_save(a->session, bpath) == 0) {
            char cmd[1400];
            snprintf(cmd, sizeof(cmd),
                "ls -t %s/*.wbus 2>/dev/null | tail -n +11 | xargs rm -f 2>/dev/null",
                bdir);
            int prc = system(cmd); (void)prc;
            printf("backup: %s\n", bpath);
        }
    }
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
/* G93: route every played note through here so CAPTURE can quantize the
 * last N seconds of the jam into material without pre-arming. */
static void daw_note(app *a, int track, uint8_t pitch, uint8_t vel) {
    if (!a->engine || track < 0) return;
    wb_engine_note(a->engine, track, pitch, vel);
    if (a->caplog && vel > 0)
        wb_caplog_note(a->caplog, a->t.song_pos, track, pitch, vel);
}

/* G93: write the captured window onto the selected track as a new clip. */
static void daw_capture(app *a) {
    if (!a->session || !a->caplog || a->selected_track < 0) return;
    double bpm = a->t.bpm > 0 ? a->t.bpm : 120.0;
    int n = wb_capture_quantize(a->caplog, a->session, a->selected_track,
                                a->t.song_pos,
                                a->capture_window > 0 ? a->capture_window : 8.0,
                                bpm);
    if (n >= 0 && a->drop_insert) {
        /* G66: INSERT drop — ripple later clips right by the new clip span */
        double t0s = a->t.song_pos / WB_SAMPLE_RATE
                   - (a->capture_window > 0 ? a->capture_window : 8.0);
        if (t0s < 0) t0s = 0;
        int shifted = wb_session_drop_place(a->session, a->selected_track,
                                            t0s * WB_SAMPLE_RATE,
                                            (a->capture_window > 0 ? a->capture_window : 8.0)
                                              * WB_SAMPLE_RATE,
                                            WB_DROP_INSERT);
        if (shifted > 0) wb_engine_set_session(a->engine, a->session);
        printf("drop: INSERT shifted %d later clip(s)\n", shifted);
    }
    if (n >= 0)
        snprintf(a->last_status, sizeof a->last_status,
                 "CAPTURED %d notes -> track %d (quantized)", n, a->selected_track);
    else
        snprintf(a->last_status, sizeof a->last_status, "CAPTURE: nothing to write");
    printf("capture: %d notes\n", n);
}

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
                daw_note(a, a->selected_track, (uint8_t)pitch, 100);
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
            /* G87: clicking a step also selects it for velocity edit */
            a->step_sel = s;
            if (a->step_pitch[ti][s] == pitch) { a->step_pitch[ti][s] = -1; return; }
            /* G87: shift+click arms a vertical velocity drag on this step */
            if (SDL_GetModState() & KMOD_SHIFT) {
                a->vel_drag_step    = s;
                a->vel_drag_start_y = y;
                a->vel_drag_start_vel = a->step_vel[ti][s];
                return;
            }
            /* G80: scale-lock snaps the drawn pitch into the current key */
            if (a->scale_lock)
                pitch = wb_scale_snap(a->scale_root, a->scale_type, pitch);
            a->step_pitch[ti][s] = pitch;
            daw_note(a, ti, (uint8_t)pitch, 100);
            /* G81: chord stamp — fill following rows with diatonic tones */
            if (a->chord_mode > 0) {
                int tones[8];
                int nt = wb_chord_tones(a->scale_root, a->scale_type,
                                        a->chord_mode, tones);
                for (int k = 0; k < nt; k++) {
                    /* each chord tone lands on its own step slot after s,
                     * pitched as a diatonic offset above the clicked note */
                    int ss = s + 1 + k;
                    int tp = pitch + (tones[k] - tones[0]);
                    if (ss < 16 && tp >= 0 && tp <= 127 &&
                        a->step_pitch[ti][ss] < 0)
                        a->step_pitch[ti][ss] = tp;
                }
            }
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

/* G10: snap a timeline sample position to the beat grid or the nearest
 * marker (whichever is closer) when snap is on. Grid = quarter note at the
 * session BPM; markers always win if within half a beat. */
static double snap_pos(app *a, double pos) {
    if (!a->snap_on || !a->session || a->session->bpm <= 0) return pos;
    double beat = 60.0 / a->session->bpm * WB_SAMPLE_RATE;
    double grid = floor(pos / beat + 0.5) * beat;      /* nearest beat */
    /* nearest marker within half a beat wins */
    for (uint32_t i = 0; i < a->session->marker_count; i++) {
        double mp = a->session->markers[i].pos;   /* already samples */
        if (fabs(mp - pos) < beat * 0.5) return mp;
    }
    return grid;
}

/* ---- G53: batch render matrix ------------------------------------------ */
/* Queue one export job per pair of consecutive SECTION markers (kind 1);
 * a leading marker with no section after it renders to session end. Output
 * names use "<base>_<MarkerLabel>.mp4" (Reaper $region wildcard spirit).
 * With batch armed, EXPORT runs these sequentially through the one-slot
 * job queue via batch_tick(). */
static void batch_queue_from_markers(app *a) {
    if (!a->session) return;
    a->batch.n = 0; a->batch.i = 0;
    const char *base = a->vid_export[0] ? a->vid_export
                                        : "/tmp/bigmac_export.mp4";
    char stem[340];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = 0;
    const char *ext = strrchr(base, '.');
    ext = ext ? ext : ".mp4";

    for (uint32_t m = 0; m < a->session->marker_count && a->batch.n < 16; m++) {
        wb_marker *mk = &a->session->markers[m];
        if (mk->kind != 1) continue;                 /* section markers only */
        double rs = mk->pos / (double)WB_SAMPLE_RATE;
        /* find the next section marker for the region end */
        double re = -1.0;
        for (uint32_t k = m + 1; k < a->session->marker_count; k++) {
            if (a->session->markers[k].kind == 1) {
                re = a->session->markers[k].pos / (double)WB_SAMPLE_RATE;
                break;
            }
        }
        double rd = re > rs ? re - rs : -1.0;       /* -1 = run to end */
        snprintf(a->batch.out[a->batch.n], sizeof(a->batch.out[0]),
                 "%.300s_%.40s%s", stem,
                 mk->label[0] ? mk->label : "region", ext);
        a->batch.rs[a->batch.n] = rs;
        a->batch.rd[a->batch.n] = rd;
        a->batch.n++;
    }
    if (a->batch.n > 0)
        snprintf(a->last_status, sizeof(a->last_status),
                 "BATCH QUEUED %d regions", a->batch.n);
}

/* Pump the batch: start the next queued job when the slot is free. */
static void batch_tick(app *a) {
    if (a->batch.n <= 0 || a->batch.i >= a->batch.n) return;
    if (wb_export_job_running(&a->ejob)) return;
    if (a->ejob.done) wb_export_job_reset(&a->ejob);   /* reap finished job */
    int rc = wb_export_job_start(&a->ejob, a->session,
                                 a->batch.out[a->batch.i],
                                 a->vid_captions_ready ? a->vid_srt : NULL,
                                 a->export_codec_h264 ? WB_VIDEO_CODEC_H264
                                                      : WB_VIDEO_CODEC_PRORES,
                                 a->batch.rs[a->batch.i],
                                 a->batch.rd[a->batch.i],
                                 a->export_res_h);
    if (rc == 0) {
        printf("batch: job %d/%d -> %s\n",
               a->batch.i + 1, a->batch.n, a->batch.out[a->batch.i]);
        a->batch.i++;
    } else if (rc == -2) {
        /* busy: retry next tick */ ;
    } else {
        fprintf(stderr, "batch: job %d failed to start\n", a->batch.i);
        a->batch.i++;
    }
    if (a->batch.i >= a->batch.n)
        snprintf(a->last_status, sizeof(a->last_status),
                 "BATCH %d/%d done", a->batch.n, a->batch.n);
}

/* ---- G43/G54: watch folder --------------------------------------------- */
/* Poll WB_WATCH_DIR (default ~/Desktop is NOT watched — opt-in dir only)
 * every 5s. New media files auto-import (G43). A .wbus file dropped in
 * triggers an immediate auto-render of the current session to
 * <watch>/<projectname>.mp4 (G54, AME watch-folder model). */
static unsigned watch_hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned)*s++; h *= 16777619u; }
    return h;
}
static void watch_poll(app *a) {
    if (!a->watch_dir[0]) return;
    time_t now = time(NULL);
    if (now - a->watch_last_poll < 5) return;      /* every 5s */
    a->watch_last_poll = now;
    char paths[32][WB_IMPORT_PATH_MAX];
    int n = wb_import_scan_dir(a->watch_dir, paths, 32);
    unsigned crc = 0;
    for (int i = 0; i < n; i++) {
        crc ^= watch_hash(paths[i]);
        size_t pl = strlen(paths[i]);
        int is_proj = pl > 5 && !strcasecmp(paths[i] + pl - 5, ".wbus");
        /* already seen? skip import/render for it */
        if (!(watch_hash(paths[i]) & ~a->watch_seen_crc)) continue;
        if (is_proj) {
            /* G54: auto-render current session beside the dropped project */
            char outp[512];
            snprintf(outp, sizeof(outp), "%.400s.mp4", paths[i]);
            if (!wb_export_job_running(&a->ejob)) {
                int rc = wb_export_job_start(&a->ejob, a->session, outp,
                                             NULL,
                                             a->export_codec_h264 ? WB_VIDEO_CODEC_H264
                                                                  : WB_VIDEO_CODEC_PRORES,
                                             -1.0, -1.0, a->export_res_h);
                printf("watch: auto-render %s -> %s (rc=%d)\n",
                       paths[i], outp, rc);
            }
        } else {
            browser_import(a, paths[i]);           /* G43: auto-import */
            printf("watch: auto-imported %s\n", paths[i]);
        }
    }
    a->watch_seen_crc = crc;
}

/* ---- G90: song mode (pattern chaining) ---------------------------------- */
/* Launch scene `sc` on every track that has a clip on that lane (same
 * semantics as a SESSION-tab scene click, without needing the click). */
static void song_launch_scene(app *a, int sc) {
    if (!a->session) return;
    for (uint32_t t = 0; t < a->session->track_count; t++) {
        wb_track *tk = &a->session->tracks[t];
        for (uint32_t c = 0; c < tk->clip_count; c++) {
            if (tk->clips[c].lane == sc) {
                wb_engine_launch(a->engine, (int)t, (int)c);
                break;
            }
        }
    }
    printf("song: scene %d\n", sc);
}

/* G90/G95: advance the chain when the bar counter crosses `song_bars`
 * boundaries. The next scene is chosen by the current scene's follow action
 * (chance-weighted), else sequential. */
static int song_next_scene(app *a) {
    if (a->song_pos < 0 || a->song_pos >= 16) return 0;
    int act = a->fa_action[a->song_pos];
    int chance = a->fa_chance[a->song_pos];
    int fires = chance >= 100 || (rand() % 100) < chance;
    switch (act) {
    case 1:                                  /* JUMP to target */
        if (fires && a->fa_target[a->song_pos] >= 0 &&
            a->fa_target[a->song_pos] < a->song_len)
            return a->fa_target[a->song_pos];
        break;
    case 2:                                  /* OTHER: any chain entry != self */
        if (fires && a->song_len > 1) {
            int pick;
            do { pick = rand() % a->song_len; } while (pick == a->song_pos);
            return pick;
        }
        break;
    case 3:                                  /* FILL: re-trigger self */
        if (fires) return a->song_pos;
        break;
    case 4:                                  /* LEGATO: next, but don't relaunch
                                                currently-playing clips — the
                                                launch call below already only
                                                starts non-playing clips, so
                                                this behaves as smooth-next */
        return (a->song_pos + 1) % a->song_len;
    default:                                 /* 0 = NEXT */
        break;
    }
    return (a->song_pos + 1) % a->song_len;
}

static void song_tick(app *a) {
    if (a->song_len <= 0 || !a->session || a->session->bpm <= 0) return;
    double spb = 60.0 / a->session->bpm * WB_SAMPLE_RATE;      /* beat */
    double bar = a->t.song_pos / (spb * 4.0);                  /* beats/bar=4 */
    int barno = (int)bar;
    int step_len = a->song_bars > 0 ? a->song_bars : 1;
    if (barno / step_len == a->song_last_bar / step_len && a->song_pos != -1)
        return;                                  /* still inside this step */
    /* wrap detection: restart the chain when it plays past its end */
    if (a->song_pos < 0 || a->song_pos >= a->song_len ||
        barno < a->song_last_bar) {
        a->song_pos = 0;
        a->song_last_bar = 0;
        song_launch_scene(a, a->song_chain[0]);
        return;
    }
    a->song_pos = song_next_scene(a);
    a->song_last_bar = barno;
    song_launch_scene(a, a->song_chain[a->song_pos]);
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
        /* G87: use the per-step velocity instead of a hardcoded 100 */
        wb_session_add_note(tr, s * step_smp, step_smp * 0.9, p,
                            a->step_vel[ti][s]);
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
            daw_checkpoint(a);   /* Wave1 G08: snapshot pre-edit */
            wb_session_add_track(a->session, "Track", 0);
            a->selected_track = ni;
            wb_engine_set_session(a->engine, a->session);
            printf("arrange: +track -> %d tracks\n", a->session->track_count);
        } else if (act == 1) {  /* MARKER at playhead */
            daw_checkpoint(a);   /* Wave1 G08 */
            wb_session_add_marker(a->session, a->t.song_pos, "M", 0);
            printf("arrange: marker at %.2fs\n", a->t.song_pos / WB_SAMPLE_RATE);
        } else if (act == 2) {  /* COMP selected marquee to lane 0 */
            if (a->selected_track >= 0 && a->sel_t1 > a->sel_t0) {
                daw_checkpoint(a);   /* Wave1 G08 */
                wb_session_comp_region(a->session, a->selected_track,
                                       a->sel_lane, a->sel_t0, a->sel_t1);
            }
            printf("arrange: comp selection -> lane 0\n");
        }
    } else if (tab == 1) {  /* PAD */
        if (act == 0) {   /* STOP ALL launches */
            for (uint32_t t = 0; t < a->session->track_count; t++)
                wb_engine_stop_launch(a->engine, (int)t);
            printf("launch: STOP ALL\n");
        } else if (act == 1) {  /* G93 CAPTURE */
            daw_capture(a);
        }
    } else if (tab == 3) {  /* SESSION */
        if (act == 0) {   /* STOP ALL launches */
            for (uint32_t t = 0; t < a->session->track_count; t++)
                wb_engine_stop_launch(a->engine, (int)t);
            /* G94: stopping the launcher while REC ARR closes the recording */
            if (a->launchrec_armed) {
                wb_launchrec_finish(a->lrec, a->t.song_pos);
                int placed = wb_launchrec_commit(a->lrec, a->session);
                snprintf(a->last_status, sizeof a->last_status,
                         "ARR RECORDING COMMITTED (%d clips)", placed);
                printf("session-rec: committed %d clips\n", placed);
                a->launchrec_armed = 0;
            }
        } else if (act == 1) {  /* G94 REC ARR toggle */
            if (!a->launchrec_armed) {
                wb_launchrec_start(a->lrec, a->session);
                a->launchrec_armed = 1;
                snprintf(a->last_status, sizeof a->last_status,
                         "REC ARR ARMED - launch clips now");
            } else {
                wb_launchrec_finish(a->lrec, a->t.song_pos);
                int placed = wb_launchrec_commit(a->lrec, a->session);
                snprintf(a->last_status, sizeof a->last_status,
                         "ARR RECORDING COMMITTED (%d clips)", placed);
                a->launchrec_armed = 0;
            }
            printf("session-rec: %s\n", a->launchrec_armed ? "armed" : "disarmed+commit");
        } else if (act == 2) {  /* G90: SONG mode — build chain from lanes, run */
            if (a->song_len > 0) {
                /* toggle off */
                a->song_len = 0; a->song_pos = -1;
                snprintf(a->last_status, sizeof a->last_status, "SONG MODE OFF");
                printf("song: off\n");
            } else {
                /* build the chain from the distinct lane indices present */
                a->song_len = 0;
                for (int sc = 0; sc < 4 && a->song_len < 16; sc++) {
                    int used = 0;
                    for (uint32_t t = 0; t < a->session->track_count && !used; t++)
                        for (uint32_t c = 0; c < a->session->tracks[t].clip_count; c++)
                            if (a->session->tracks[t].clips[c].lane == sc)
                                { used = 1; break; }
                    if (used) a->song_chain[a->song_len++] = sc;
                }
                if (a->song_len > 0) {
                    a->song_pos = -1;
                    a->song_last_bar = 0;
                    song_launch_scene(a, a->song_chain[0]);
                    a->song_pos = 0;
                    snprintf(a->last_status, sizeof a->last_status,
                             "SONG: %d scenes chained", a->song_len);
                    printf("song: chained %d scenes\n", a->song_len);
                } else {
                    snprintf(a->last_status, sizeof a->last_status,
                             "SONG: no scene clips to chain");
                }
            }
        }
    } else if (tab == 2) {  /* STEP */
        if (act == 0) {  /* CLEAR pattern on selected track */
            int ti = a->selected_track;
            if (ti >= 0) for (int s = 0; s < 16; s++) a->step_pitch[ti][s] = -1;
            printf("step: cleared pattern (track %d)\n", ti);
        } else if (act == 1) {  /* COMMIT to clip */
            step_commit_to_clip(a);
        } else if (act == 2) {  /* G93 CAPTURE */
            daw_capture(a);
        } else if (act == 3) {  /* G91: step-fill — shift = every 4th, plain = every 2nd, alt = random */
            int ti = a->selected_track;
            if (ti >= 0) {
                SDL_Keymod mod = SDL_GetModState();
                int row = 0;   /* fill with the root row pitch (C4 lane) */
                if (mod & KMOD_ALT) {
                    for (int s = 0; s < 16; s++)
                        a->step_pitch[ti][s] = (rand() % 3 == 0) ? 60 - (rand() % 8) : -1;
                    printf("step: random fill (track %d)\n", ti);
                } else {
                    int stride = (mod & KMOD_SHIFT) ? 4 : 2;
                    for (int s = 0; s < 16; s += stride)
                        a->step_pitch[ti][s] = 60 - row;
                    printf("step: filled every %dth (track %d)\n", stride, ti);
                }
                snprintf(a->last_status, sizeof a->last_status,
                         "STEP FILL (track %d)", ti);
            }
        } else if (act == 4) {  /* G92: retrig — repeat the selected step's
                                   note every step until the next filled step
                                   or pattern end, halving velocity each echo,
                                   accenting (restoring full vel) every 4th */
            int ti = a->selected_track;
            int s0 = a->step_sel;
            if (ti >= 0 && s0 >= 0 && a->step_pitch[ti][s0] >= 0) {
                int pitch = a->step_pitch[ti][s0];
                int vel0  = a->step_vel[ti][s0];
                /* find the run end: next already-filled step or pattern end */
                int end = 16;
                for (int s = s0 + 1; s < 16; s++)
                    if (a->step_pitch[ti][s] >= 0) { end = s; break; }
                int v = vel0;
                for (int s = s0 + 1; s < end; s++) {
                    a->step_pitch[ti][s] = pitch;
                    if ((s - s0) % 4 == 0)
                        v = vel0;                        /* accent: back to full */
                    else
                        v = v * 60 / 100;                /* decay to 60% */
                    if (v < 8) v = 8;
                    a->step_vel[ti][s] = v;
                    a->step_prob[ti][s] = 100;
                }
                snprintf(a->last_status, sizeof a->last_status,
                         "RETRIG %d steps from %d", end - s0 - 1, s0);
                printf("step: retrig track %d from step %d to %d\n",
                       ti, s0, end);
            } else {
                snprintf(a->last_status, sizeof a->last_status,
                         "RETRIG: click a step first");
            }
        }
    } else {  /* video tabs 4..7 */
        if (act == 0) {  /* IMPORT demo */
            const char *dv = "/Users/waefrebeorn/Documents/big-mac/test_media/demo.mp4";
            if (access(dv, F_OK) != 0) dv = "/Users/waefrebeorn/Videos/demo.mp4";
            if (access(dv, F_OK) == 0) { daw_checkpoint(a); video_import(a, dv); }
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
        } else if (act == 2) {  /* EXPORT -> background render queue (G38) */
            if (a->vid_has_clip) {
                /* G53: shift+EXPORT = batch render per marker region */
                if (SDL_GetModState() & KMOD_SHIFT) {
                    if (!a->vid_export[0]) snprintf(a->vid_export, sizeof(a->vid_export),
                             "/tmp/bigmac_export_%d.mp4", (int)(a->t.song_pos/WB_SAMPLE_RATE*100));
                    batch_queue_from_markers(a);
                    return;
                }
                if (!a->vid_export[0]) snprintf(a->vid_export, sizeof(a->vid_export),
                         "/tmp/bigmac_export_%d.mp4", (int)(a->t.song_pos/WB_SAMPLE_RATE*100));
                /* G39: range — IN marker (SDLK_SEMICOLON) to playhead; whole otherwise */
                double rs = -1.0, rd = -1.0;
                if (a->export_range_mode == 1 && a->io_out <= a->io_in
                    && a->t.song_pos > a->io_in) {
                    rs = a->io_in / (double)WB_SAMPLE_RATE;
                    rd = (a->t.song_pos - a->io_in) / (double)WB_SAMPLE_RATE;
                }
                int rc = wb_export_job_start(&a->ejob, a->session, a->vid_export,
                                             a->vid_captions_ready ? a->vid_srt : NULL,
                                             a->export_codec_h264 ? WB_VIDEO_CODEC_H264
                                                                  : WB_VIDEO_CODEC_PRORES,
                                             rs, rd, a->export_res_h);
                if (rc == 0)
                    snprintf(a->last_status, sizeof a->last_status,
                             "EXPORT QUEUED (%s)", rd > 0 ? "range" : "whole");
                else if (rc == -2)
                    snprintf(a->last_status, sizeof a->last_status,
                             "EXPORT BUSY - cancel or wait");
                else
                    snprintf(a->last_status, sizeof a->last_status,
                             "EXPORT FAILED to start");
                printf("video: export queued rc=%d\n", rc);
            }
        } else if (act == 3 && a->tab == 5) {  /* EDIT: FREEZE live perf -> timeline clip */
            if (!a->perf) { fprintf(stderr, "perf: no engine\n"); return; }
            int ti = -1;
            for (uint32_t t = 0; t < a->session->track_count; t++)
                if (a->session->tracks[t].kind == WB_TRACK_KIND_VIDEO) { ti = (int)t; break; }
            if (ti < 0) { fprintf(stderr, "freeze: no video track\n"); return; }
            double dur = a->t.song_pos / WB_SAMPLE_RATE;
            if (dur <= 0.0) dur = 4.0;
            if (a->perf_recording) { wb_perf_record_stop(a->perf); a->perf_recording = 0; }
            wb_perf_set_clock(a->perf, 0);
            wb_perfclip *pc = wb_perfclip_snapshot(a->session, a->perf, 0, dur);
            if (!pc) { fprintf(stderr, "freeze: snapshot failed\n"); return; }
            int ci = wb_session_add_perf_clip(a->session, ti, pc,
                                              0.0, dur);
            if (ci >= 0) {
                snprintf(a->last_status, sizeof(a->last_status),
                         "PERF FROZEN: track %d clip %d (%.1fs)", ti, ci, dur);
                printf("freeze: perf -> track %d clip %d @0 (%.1fs)\n", ti, ci, dur);
            } else {
                fprintf(stderr, "freeze: add_perf_clip failed\n");
            }
        } else if (act == 3 && a->tab == 6) {  /* CAPTIONS: DELIVER = normalized master */
            if (!a->vid_has_clip) return;
            char out[512];
            snprintf(out, sizeof(out), "/tmp/bigmac_master_%d.mov", (int)getpid());
            int rc = wb_video_export_delivery(a->session, a->engine, out,
                                              a->vid_captions_ready ? a->vid_srt : NULL,
                                              WB_VIDEO_CODEC_PRORES, NULL, -14.0);
            if (rc == 0)
                snprintf(a->last_status, sizeof(a->last_status),
                         "DELIVERED: %.80s (-14 LUFS)", out);
            else
                snprintf(a->last_status, sizeof(a->last_status),
                         "DELIVER FAILED rc=%d", rc);
            printf("delivery: %s rc=%d\n", out, rc);
        } else if (act == 3 && a->tab == 7) {  /* EXPORT: codec toggle H264 <-> ProRes */
            a->export_codec_h264 = !a->export_codec_h264;
        }
    }
}

/* ---- R035: per-frame performance tick (STEP playback + PAD flash) ---- */
static void perf_tick(app *a) {
    /* decay PAD flash counters (G62: skipped entirely under reduced motion) */
    for (int i = 0; i < 32; i++) if (a->pad_flash[i] > 0) a->pad_flash[i]--;
    /* STEP sequencer: fire the selected track's active steps on each 16th */
    if (a->tab == 2 && a->selected_track >= 0 && a->session) {
        double bpm = a->t.bpm > 0 ? a->t.bpm : 120.0;
        double spstep = (60.0/bpm)/4.0;   /* 16th-note seconds */
        int cur = (int)(a->t.song_pos / WB_SAMPLE_RATE / spstep) % 16;
        /* G89: odd steps fire LATE by swing*sixteenth (Roger Linn spec),
         * matching the transport scheduler so STEP and arrangement agree. */
        double sw = a->session ? a->session->swing : 0.0;
        double off_samp = wb_swing_offset(bpm, sw,
                                          fmod(a->t.song_pos,
                                               spstep*16.0*WB_SAMPLE_RATE));
        double stepstart = (double)a->t.song_pos / WB_SAMPLE_RATE / spstep;
        double frac_into_step = stepstart - floor(stepstart);
        double need_frac = off_samp / WB_SAMPLE_RATE / spstep;
        if (cur != a->last_step && frac_into_step >= need_frac) {
            a->last_step = cur;
            int ti = a->selected_track;
            int p = a->step_pitch[ti][cur];
            if (p >= 0) {
                /* G88: probabilistic triggering — roll against step_prob */
                int prob = a->step_prob[ti][cur];
                int fire = (prob >= 100) ||
                           ((rand() % 100) < prob);
                if (fire)
                    /* G87: fire with the per-step velocity */
                    daw_note(a, ti, (uint8_t)p,
                             (uint8_t)a->step_vel[ti][cur]);
            }
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
    /* G94: while REC ARR is armed, poll the session launcher each frame so
     * launch/stop transitions become recorded spans. */
    if (a->lrec && a->launchrec_armed && a->session)
        wb_launchrec_poll(a->lrec, a->session, a->engine, a->t.song_pos);
    /* R043-G7: live ticks for the upper-tier views (CGI rotation + AGI pipeline) */
    if (a->cgi && wb_workspace_cgi_active(a->ws)) wb_cgi_scene_tick(a->cgi, 1.0/60.0);
    if (a->agi && wb_workspace_agi_active(a->ws)) wb_agi_tick(a->agi, 1.0/60.0);
    song_tick(a);   /* G90: pattern chaining / song mode */
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
                    /* G25: RIGHT-click cycles the automation mode */
                    if (b.button == SDL_BUTTON_RIGHT) {
                        wb_automation_lane *mlane = NULL;
                        for (uint32_t l = 0; l < a->session->automation_count; l++)
                            if (a->session->automation[l]->target == ti &&
                                !strcmp(a->session->automation[l]->param, "volume"))
                            { mlane = a->session->automation[l]; break; }
                        if (!mlane) mlane = wb_session_add_automation(a->session, "volume", ti);
                        static const char *modes[] = { "READ", "WRITE", "TOUCH", "LATCH" };
                        mlane->mode = (mlane->mode + 1) % 4;
                        snprintf(a->last_status, sizeof(a->last_status),
                                 "AUTO MODE T%d: %s", ti, modes[mlane->mode]);
                        printf("automode: track %d -> %s\n", ti, modes[mlane->mode]);
                        return;
                    }
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
                case BTN_SNAP:    a->snap_on = !a->snap_on;
                          wb_engine_set_snap(a->engine, a->snap_on); break;
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
                case 62000:   /* G92: retrig on the selected step (STEP tab) */
                    handle_action(a, 4); break;
                case 63000: { /* G95: cycle follow action + chance for the
                                 current song-mode chain position. Left = next
                                 action; right = chance 25->50->100->0. */
                    int p = a->song_pos >= 0 ? a->song_pos : 0;
                    if (b.button == SDL_BUTTON_RIGHT) {
                        a->fa_chance[p] = a->fa_chance[p] == 0 ? 25 :
                                          a->fa_chance[p] == 25 ? 50 :
                                          a->fa_chance[p] == 50 ? 100 : 0;
                    } else {
                        a->fa_action[p] = (a->fa_action[p] + 1) % 5;
                    }
                    printf("song: scene %d follow=%d chance=%d%%\n",
                           p, a->fa_action[p], a->fa_chance[p]);
                    break;
                }
                case BTN_WS4+100: {  /* CAPTIONS tab: CHAP — chapters from markers */
                    if (a->session && a->session->marker_count >= 2) {
                        char buf[4096];
                        if (wb_delivery_chapters(a->session, buf, sizeof(buf)) == 0) {
                            snprintf(a->last_status, sizeof(a->last_status),
                                     "CHAPTERS: %.40s...", buf);
                            printf("chapters:\n%s\n", buf);
                        }
                    } else {
                        snprintf(a->last_status, sizeof(a->last_status),
                                 "CHAPTERS: need >= 2 markers");
                    }
                    break;
                }
                case 9101: {  /* G46: SRT OUT — write current captions beside the source */
                    if (a->vid_captions_ready && a->vid_srt[0]) {
                        char outp[600];
                        if (a->vid_source[0]) {
                            snprintf(outp, sizeof(outp), "%.500s", a->vid_source);
                            char *dot = strrchr(outp, '.');
                            if (dot) *dot = 0;
                            strlcat(outp, ".srt", sizeof(outp));
                        } else {
                            snprintf(outp, sizeof(outp), "/tmp/bigmac_export.srt");
                        }
                        FILE *fi = fopen(a->vid_srt, "rb"), *fo = fopen(outp, "wb");
                        if (fi && fo) {
                            char cp[4096]; size_t nr;
                            while ((nr = fread(cp, 1, sizeof(cp), fi)) > 0)
                                fwrite(cp, 1, nr, fo);
                            snprintf(a->last_status, sizeof(a->last_status),
                                     "SRT SAVED: %.44s", outp);
                            printf("srt-export: %s -> %s\n", a->vid_srt, outp);
                        } else {
                            snprintf(a->last_status, sizeof(a->last_status),
                                     "SRT SAVE FAILED");
                        }
                        if (fi) fclose(fi);
                        if (fo) fclose(fo);
                    } else {
                        snprintf(a->last_status, sizeof(a->last_status),
                                 "SRT OUT: no captions yet (run CAPTIONS)");
                    }
                    break;
                }
                case 9102:    /* G46: SRT IN — open browser (routes .srt files) */
                    a->browser_open = 1;
                    break;
                case 9103: {  /* G44: TITLE — arm text entry; in/out = loop brace */
                    a->title_entry = !a->title_entry;
                    if (a->title_entry) {
                        a->title_text[0] = 0;
                        a->title_in = a->t.song_pos / (double)WB_SAMPLE_RATE;
                        a->title_out = a->title_in + 4.0;
                        snprintf(a->last_status, sizeof(a->last_status),
                                 "TITLE: type text, ENTER commits");
                    }
                    break;
                }
                case 9104: {  /* G44: TBURN — bake the title into the video */
                    if (!a->vid_has_clip || !a->title_text[0]) {
                        snprintf(a->last_status, sizeof(a->last_status),
                                 "TBURN: set a title first (TITLE)");
                        break;
                    }
                    char outp[600];
                    snprintf(outp, sizeof(outp), "%.450s_titled.mp4",
                             a->vid_source[0] ? a->vid_source
                                              : "/tmp/bigmac_export.mp4");
                    int rc = wb_captions_burn_title(
                        a->vid_source, outp, a->title_text,
                        a->title_in, a->title_out, a->title_pos, 0);
                    snprintf(a->last_status, sizeof(a->last_status),
                             rc == 0 ? "TITLE BURNED: %.40s" : "TITLE BURN FAILED",
                             outp);
                    printf("title: burn rc=%d -> %s\n", rc, outp);
                    break;
                }
                case BTN_OVERVIEW:   /* R040: begin scroll-drag on the overview strip */
                    a->ov_drag = 1;
                    ov_scroll_to(a, b.x);
                    break;
                case 70001: { /* G10: drag loop-IN brace on ruler */
                    double sec = a->view_start/WB_SAMPLE_RATE + (b.x - GUTTER_W)*(a->visible_secs/ARRANG_W);
                    a->ruler_in = sec * WB_SAMPLE_RATE;
                    if (a->ruler_out <= a->ruler_in) a->ruler_out = a->ruler_in + 44100.0;
                    a->loop_on = 1; a->ruler_drag = 1;
                    wb_engine_set_loop(a->engine, a->ruler_in, a->ruler_out); break; }
                case 70002: { /* G10: drag loop-OUT brace on ruler */
                    double sec = a->view_start/WB_SAMPLE_RATE + (b.x - GUTTER_W)*(a->visible_secs/ARRANG_W);
                    a->ruler_out = sec * WB_SAMPLE_RATE;
                    if (a->ruler_out <= a->ruler_in) a->ruler_in = a->ruler_out - 44100.0;
                    a->loop_on = 1; a->ruler_drag = 2;
                    wb_engine_set_loop(a->engine, a->ruler_in, a->ruler_out); break; }
                case (BTN_OVERVIEW-1000): { /* G10: click ruler to set IN..OUT */
                    if (!a->loop_on) break;
                    double sec = a->view_start/WB_SAMPLE_RATE + (b.x - GUTTER_W)*(a->visible_secs/ARRANG_W);
                    double s = sec * WB_SAMPLE_RATE;
                    if (a->ruler_in <= 0) a->ruler_in = s;
                    else if (a->ruler_out <= a->ruler_in) a->ruler_out = s;
                    else { a->ruler_in = s; a->ruler_out = 0; }
                    wb_engine_set_loop(a->engine, a->ruler_in, a->ruler_out); break; }
                case BTN_UNDO: {     /* Wave1 G08 */
                    if (a->undo && wb_undo_undo(a->undo, &a->session) == 1) {
                        wb_engine_set_session(a->engine, a->session);
                        snprintf(a->last_status, sizeof(a->last_status), "UNDO");
                        printf("undo: restored (depth %d)\n", wb_undo_depth(a->undo));
                    } else snprintf(a->last_status, sizeof(a->last_status), "nothing to undo");
                    break;
                }
                case BTN_REDO: {     /* Wave1 G08 */
                    if (a->undo && wb_undo_redo(a->undo, &a->session) == 1) {
                        wb_engine_set_session(a->engine, a->session);
                        snprintf(a->last_status, sizeof(a->last_status), "REDO");
                        printf("redo: re-applied (depth %d)\n", wb_undo_redo_depth(a->undo));
                    } else snprintf(a->last_status, sizeof(a->last_status), "nothing to redo");
                    break;
                }
                case BTN_BROWSE:     /* Wave1 G01: open/close the media browser */
                    browser_scan(a);
                    a->browser_open = 1;
                    break;
                case BTN_BROWSER_CANCEL:
                    a->browser_open = 0;
                    break;
                case BTN_SWING_M:   /* G89: swing -5% of a 16th, clamp 0..0.6 */
                    if (a->session) {
                        a->session->swing -= 0.05;
                        if (a->session->swing < 0.0) a->session->swing = 0.0;
                    }
                    break;
                case BTN_SWING_P:
                    if (a->session) {
                        a->session->swing += 0.05;
                        if (a->session->swing > 0.6) a->session->swing = 0.6;
                    }
                    break;
                case BTN_SCALE_ROOT:   /* G80: cycle scale root 0..11 */
                    a->scale_root = (a->scale_root + 1) % 12;
                    printf("scale: %s %s\n", scale_root_name(a->scale_root),
                           (const char *[]){"major","minor","dorian","mixolydian","phrygian"}[a->scale_type%5]);
                    break;
                case BTN_SCALE_TYPE:   /* G80: cycle scale type */
                    a->scale_type = (a->scale_type + 1) % 5;
                    printf("scale: %s\n", scale_name(a->scale_root, a->scale_type));
                    break;
                case BTN_LOCK:         /* G80: toggle scale-lock */
                    a->scale_lock = !a->scale_lock;
                    snprintf(a->last_status, sizeof(a->last_status),
                             "SCALE-LOCK %s", a->scale_lock ? "ON" : "OFF");
                    printf("scale-lock: %s\n", a->scale_lock ? "ON" : "OFF");
                    break;
                case BTN_CHORD:        /* G81: cycle chord stamp OFF/triad/7th/9th */
                    a->chord_mode = (a->chord_mode + 1) % 4;
                    printf("chord: %s\n", a->chord_mode ?
                           (a->chord_mode==1?"TRIAD":a->chord_mode==2?"7TH":"9TH") : "off");
                    break;
                default:
                    if (id >= BTN_BROWSER_ROW0 && id < BTN_BROWSER_ROW0 + 128) {
                        /* Wave1 G01: click a browser row -> import that file */
                        int idx = id - BTN_BROWSER_ROW0;
                        if (idx < a->browser_count) {
                            char p[WB_IMPORT_PATH_MAX];
                            snprintf(p, sizeof(p), "%s", a->browser_paths[idx]);
                            a->browser_open = 0;
                            browser_import(a, p);
                        }
                    } else if (id >= 500 && id < 600) {   /* G09: track name */
                        int ti = id - 500;
                        Uint32 now = SDL_GetTicks();
                        if (a->last_click_track == ti &&
                            now - a->last_click_ms < 450) {
                            a->rename_armed = 1; a->rename_track = ti;
                            snprintf(a->last_status, sizeof a->last_status,
                                     "RENAME track %d: type, ENTER commits", ti);
                        }
                        a->last_click_track = ti; a->last_click_ms = now;
                    } else if (id >= 6000 && id < 6100) { /* G09: REC arm */
                        int ti = id - 6000;
                        if (ti < (int)a->session->track_count) {
                            a->session->tracks[ti].rec_armed =
                                !a->session->tracks[ti].rec_armed;
                            wb_engine_set_session(a->engine, a->session);
                            printf("track %d rec %s\n", ti,
                                   a->session->tracks[ti].rec_armed ? "ARMED" : "off");
                        }
                    } else if (id >= 7000 && id < 7100) { /* G09: delete track */
                        int ti = id - 7000;
                        if (ti < (int)a->session->track_count &&
                            a->session->track_count > 1) {
                            daw_checkpoint(a);
                            char nm[64]; snprintf(nm, sizeof nm, "%.60s",
                                                  a->session->tracks[ti].name);
                            wb_session_remove_track(a->session, (uint32_t)ti);
                            wb_engine_set_session(a->engine, a->session);
                            snprintf(a->last_status, sizeof a->last_status,
                                     "DELETED track %.40s", nm);
                            printf("track: removed '%s'\n", nm);
                        }
                    } else if ((id >= 7300 && id < 7400) ||
                               (id >= 7400 && id < 7500)) { /* G09: reorder */
                        int up = id >= 7300 && id < 7400;
                        int ti = (id % 100);
                        int j = ti + (up ? -1 : 1);
                        if (j >= 0 && j < (int)a->session->track_count) {
                            daw_checkpoint(a);
                            wb_session_move_track(a->session, (uint32_t)ti,
                                                  up ? -1 : 1);
                            wb_engine_set_session(a->engine, a->session);
                            printf("track: moved %d -> %d\n", ti, j);
                        }
                    } else if (id >= 1000 && id < 2000) {  /* per-track Mute in gutter */
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
                    } else if (id >= 9000 && id < 9100) {  /* G38/G39/G40 EXPORT settings */
                        switch (id) {
                            case 9001: a->export_range_mode = !a->export_range_mode; break;
                            case 9002: a->export_res_h = 480; break;
                            case 9003: a->export_res_h = 720; break;
                            case 9004: a->export_res_h = 0;   break;  /* native 1080 */
                            case 9005: wb_export_job_cancel(&a->ejob);
                                       snprintf(a->last_status, sizeof a->last_status,
                                                "CANCEL REQUESTED"); break;
                        }
                        printf("export: mode=%d res=%d\n", a->export_range_mode, a->export_res_h);
                    } else if (id == 9100) {  /* G52: cycle delivery preset */
                        a->delivery_profile_idx = (a->delivery_profile_idx + 1) % G_DELIVERY_N;
                        const wb_delivery_profile *dp =
                            daw_delivery_profile(a->delivery_profile_idx);
                        snprintf(a->last_status, sizeof a->last_status,
                                 "PROFILE: %s (%.0f LUFS)",
                                 g_delivery_cycle[a->delivery_profile_idx],
                                 dp ? dp->lufs : -14.0);
                        printf("delivery: profile %s (%.1f LUFS, TP %.1f)\n",
                               g_delivery_cycle[a->delivery_profile_idx],
                               dp ? dp->lufs : -14.0, dp ? dp->tp_ceiling : -1.5);
                    } else if (id == 9101) {  /* G52: DELIVER with chosen profile */
                        const wb_delivery_profile *dp =
                            daw_delivery_profile(a->delivery_profile_idx);
                        if (!a->vid_has_clip) {
                            snprintf(a->last_status, sizeof a->last_status,
                                     "DELIVER: no video loaded");
                            return;
                        }
                        char out[512];
                        snprintf(out, sizeof(out), "/tmp/bigmac_master_%d.mov",
                                 (int)getpid());
                        int rc = wb_video_export_delivery(
                            a->session, a->engine, out,
                            a->vid_captions_ready ? a->vid_srt : NULL,
                            a->export_codec_h264 ? WB_VIDEO_CODEC_H264
                                                 : WB_VIDEO_CODEC_PRORES,
                            NULL, dp ? dp->lufs : -14.0);
                        if (rc == 0)
                            snprintf(a->last_status, sizeof a->last_status,
                                     "DELIVERED [%s %.0f LUFS]: %.60s",
                                     g_delivery_cycle[a->delivery_profile_idx],
                                     dp ? dp->lufs : -14.0, out);
                        else
                            snprintf(a->last_status, sizeof a->last_status,
                                     "DELIVER FAILED rc=%d", rc);
                        printf("deliver[%s]: rc=%d out=%s\n",
                               g_delivery_cycle[a->delivery_profile_idx], rc, out);
                    } else if (id == 9102) {  /* G41: STEMS export */
                        if (!a->session || a->session->track_count == 0) {
                            snprintf(a->last_status, sizeof a->last_status,
                                     "STEMS: empty session");
                            return;
                        }
                        snprintf(a->last_status, sizeof a->last_status,
                                 "STEMS: rendering tracks...");
                        char dir[256];
                        snprintf(dir, sizeof dir, "/tmp/bigmac_stems_%d",
                                 (int)getpid());
                        int ns = wb_delivery_export_stems(a->session, dir);
                        if (ns >= 0)
                            snprintf(a->last_status, sizeof a->last_status,
                                     "STEMS: %d files in %.80s", ns, dir);
                        else
                            snprintf(a->last_status, sizeof a->last_status,
                                     "STEMS FAILED");
                        printf("stems: %d -> %s\n", ns, dir);
                    } else if (id >= 3000 && id < 4000) {  /* scene-launch column (launch all tracks' clip on that scene) */
                        int sc = id - 3000;
                        for (uint32_t t = 0; t < a->session->track_count; t++) {
                            wb_track *tk = &a->session->tracks[t];
                            for (uint32_t c = 0; c < tk->clip_count; c++)
                                if (tk->clips[c].lane == sc) { wb_engine_launch(a->engine, (int)t, (int)c); break; }
                        }
                        printf("session: launch scene %d (all tracks)\n", sc);
                    } else if (id >= 40000 && id < 50000) {  /* G30/G74: mixer send controls */
                        int ti = (id - 40000) / 8;
                        int rem = (id - 40000) % 8;
                        int si = rem / 4, what = rem % 4;
                        if (ti < (int)a->session->track_count && si >= 0 && si < 2) {
                            wb_track *tr = &a->session->tracks[ti];
                            if (what == 0) {   /* minus: -10% */
                                tr->send_level[si] -= 0.10f;
                                if (tr->send_level[si] < 0.0f) tr->send_level[si] = 0.0f;
                            } else if (what == 1) {  /* plus: +10% */
                                tr->send_level[si] += 0.10f;
                                if (tr->send_level[si] > 1.0f) tr->send_level[si] = 1.0f;
                                /* default target: first bus track */
                                if (tr->send_target[si] < 0)
                                    for (uint32_t bt = 0; bt < a->session->track_count; bt++)
                                        if (a->session->tracks[bt].kind == 2) {
                                            tr->send_target[si] = (int)bt; break;
                                        }
                            } else if (what == 2) {  /* PRE/POST toggle (G74) */
                                tr->send_pre[si] = !tr->send_pre[si];
                            } else if (what == 3) {  /* cycle target among bus tracks */
                                int nbuses = 0;
                                for (uint32_t bt = 0; bt < a->session->track_count; bt++)
                                    if (a->session->tracks[bt].kind == 2) nbuses++;
                                if (nbuses > 0) {
                                    int cur = tr->send_target[si];
                                    /* next bus after cur (wraps; -1 -> first bus) */
                                    int nxt = -1;
                                    for (int step = 0; step < nbuses; step++) {
                                        cur = (cur + 1);
                                        if (cur >= (int)a->session->track_count) cur = 0;
                                        if (a->session->tracks[cur].kind == 2) { nxt = cur; break; }
                                        if (step == nbuses-1) break;
                                    }
                                    tr->send_target[si] = nxt;
                                }
                            }
                        }
                    } else if (id >= 60000 && id < 60100) {  /* G31: remove FX */
                        int ti = (id - 60000) / 8;
                        int s  = (id - 60000) % 8;
                        if (ti < (int)a->session->track_count &&
                            s > 0 /* never clear the instrument slot */) {
                            wb_track *tr = &a->session->tracks[ti];
                            if (tr->inserts[s].id[0]) {
                                char nm[16];
                                snprintf(nm, sizeof(nm), "%s", tr->inserts[s].id);
                                wb_session_set_insert(a->session, ti, s, NULL);
                                wb_engine_set_session(a->engine, a->session);
                                snprintf(a->last_status, sizeof(a->last_status),
                                         "FX REMOVED %d:%.12s", s, nm);
                            }
                        }
                    } else if (id >= 61000 && id < 61100) {  /* G31: add FX */
                        int ti = (id - 61000) / 8;
                        if (ti < (int)a->session->track_count &&
                            ti == a->selected_track) {
                            static const char *palette[] =
                                { "eq", "chorus", "delay", "reverb",
                                  "saturation", "gate" };
                            const int NP = (int)(sizeof(palette)/sizeof(palette[0]));
                            wb_track *tr = &a->session->tracks[ti];
                            int slot = -1;
                            for (int s2 = 1; s2 < WB_MAX_INSERT_SLOTS; s2++)
                                if (!tr->inserts[s2].id[0]) { slot = s2; break; }
                            if (slot > 0) {
                                /* cycle: pick palette entry after any existing
                                 * same-palette unit so repeated +FX varies */
                                int pick = a->fx_add_cycle++ % NP;
                                wb_session_set_insert(a->session, ti, slot,
                                                      palette[pick]);
                                wb_engine_set_session(a->engine, a->session);
                                snprintf(a->last_status,
                                         sizeof(a->last_status),
                                         "FX ADDED %d:%.12s", slot,
                                         palette[pick]);
                            } else {
                                snprintf(a->last_status,
                                         sizeof(a->last_status),
                                         "FX RACK FULL");
                            }
                        }
                    } else if (id >= 50000 && id < 50100) {  /* G75: sidechain routing */
                        int ti = (id - 50000) / 8;
                        int s  = (id - 50000) % 8;
                        if (ti < (int)a->session->track_count &&
                            s < WB_MAX_INSERT_SLOTS) {
                            wb_track *tr = &a->session->tracks[ti];
                            /* cycle: -1 -> 0 -> 1 -> ... -> last -> -1.
                             * Skip self-routing (a track can't key itself). */
                            int nt = (int)a->session->track_count;
                            int nxt = tr->sidechain[s] + 1;
                            if (nxt >= nt) nxt = -1;
                            else if (nxt == ti) nxt++;
                            if (nxt >= nt) nxt = -1;
                            tr->sidechain[s] = nxt;
                            wb_engine_set_insert_sidechain(a->engine, ti, s, nxt);
                            snprintf(a->last_status, sizeof a->last_status,
                                     "SC %d:%s <- %s", s, tr->inserts[s].id,
                                     nxt >= 0 ? a->session->tracks[nxt].name
                                              : "off");
                        }
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
                    /* G13: RIGHT-click the clip BODY (h==6) cycles its color
                     * slot 0..7 in the side-table (Ableton labeling). */
                    if (h == 6 && b.button == SDL_BUTTON_RIGHT && te) {
                        te->color = (te->color + 1) % 8;
                        printf("clip color: track %d clip %d -> slot %d\n",
                               ti, c, te->color);
                        snprintf(a->last_status, sizeof(a->last_status),
                                 "CLIP COLOR %d", te->color);
                        return;
                    }
                    /* G64: RIGHT-click a fade handle cycles the crossfade
                     * curve type: linear -> equal-power -> smoothstep. */
                    if ((h == 2 || h == 3) && b.button == SDL_BUTTON_RIGHT && te) {
                        static const char *names[3] = { "linear", "equal-power", "smoothstep" };
                        te->curve = (te->curve + 1) % 3;
                        printf("fade curve: track %d clip %d -> %s\n",
                               ti, c, names[te->curve]);
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
        /* G16: razor armed -> split ALL clips under this x, then auto-exit */
        if (a->razor_on && !a->trim_mode &&
            (a->tab == 0 || a->tab == 5)) {
            double t_sec = x_to_sample(a, b.x) / WB_SAMPLE_RATE;
            int all = (SDL_GetModState() & KMOD_SHIFT) ? 1 : 0;
            int n = wb_session_razor_split_all_at_time(a->session, t_sec,
                                                       a->selected_track, all);
            if (n > 0) {
                wb_engine_set_session(a->engine, a->session);
                snprintf(a->last_status, sizeof(a->last_status),
                         "RAZOR: %d cut(s) @ %.2fs", n, t_sec);
                printf("razor: %d clip(s) split at %.2fs%s\n", n, t_sec,
                       all ? " (all tracks)" : "");
            } else {
                printf("razor: nothing under blade at %.2fs\n", t_sec);
            }
            a->razor_on = 0;   /* one cut per arm */
            return;
        }
        /* G15: clicking near a cut on the EDIT timeline enters TRIM MODE
         * pinned to that boundary (nearest edge of that clip). */
        if (a->tab == 5 && a->vid_has_clip && !a->trim_mode &&
            a->vid_track >= 0 && a->vid_track < (int)a->session->track_count) {
            double t_sec = x_to_sample(a, b.x) / WB_SAMPLE_RATE;
            double best_px = 7.0 / arr_px_per_sec(a);   /* ~7px window */
            int hit = -1, hedge = 0;
            wb_track *vtr = &a->session->tracks[a->vid_track];
            for (uint32_t c = 0; c < vtr->clip_count; c++) {
                double cs = vtr->clips[c].start;
                double ce = cs + vtr->clips[c].length;
                if (fabs(t_sec - cs) < best_px) { best_px = fabs(t_sec - cs); hit = (int)c; hedge = 0; }
                if (fabs(ce - t_sec) < best_px) { best_px = fabs(ce - t_sec); hit = (int)c; hedge = 1; }
            }
            if (hit >= 0) {
                trim_mode_enter(a, hit);
                a->trim_edge = hedge;
                printf("trim: clicked cut of clip %d (%s)\n", hit,
                       hedge == 0 ? "in" : "out");
                return;
            }
        }
        /* R035: PAD / STEP / SESSION are distinct performance views */
        if (a->tab == 1) { pad_click(a, b.x, b.y); return; }
        if (a->tab == 2) { step_click(a, b.x, b.y); return; }
        if (a->tab == 3) { session_click(a, b.x, b.y); return; }
        /* G24: keyframe editor intercepts clicks on the FUSION tier */
        if (a->tab == 5 && wb_workspace_fusion_active(a->ws) &&
            kf_click(a, b.x, b.y, b.button))
            return;
        /* R050: on the 3D-CGI tier a left-drag orbits the scene. */
        if (a->cgi && wb_workspace_cgi_active(a->ws) && a->tab == 5) {
            a->cgi_dragging = 1;
            a->cgi_last_x = b.x;
            a->cgi_last_y = b.y;
            return;
        }
        /* seek playhead to click position (scrub-on-click; G10: snapped) */
        double pos = snap_pos(a, x_to_sample(a, b.x));
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
    /* Wave1 G01: browser overlay scrolls the file list */
    if (a->browser_open) {
        a->browser_scroll -= w.y;
        int max_scroll = a->browser_count - 1;
        if (max_scroll < 0) max_scroll = 0;
        if (a->browser_scroll > max_scroll) a->browser_scroll = max_scroll;
        if (a->browser_scroll < 0) a->browser_scroll = 0;
        return;
    }
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
    /* G24: keyframe drag — retime/revalue the dragged key */
    if (a->kf_drag >= 0 && a->kf_track) {
        wb_keyframe k;
        if (wb_param_track_key_index(a->kf_track, a->kf_drag, &k) == 0) {
            double nt; float nv;
            screen_to_kf(a, m.x, m.y, &nt, &nv);
            wb_param_track_move_key(a->kf_track, k.t, nt, nv);
        }
        return;
    }
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
        wb_clip_edit_table *et = wb_engine_clip_edit(a->engine);
        wb_clip_edit *ce = wb_clip_edit_get(et, a->hd_track, a->hd_clip);
        switch (a->handle_drag) {
        case 0: {  /* LEFT trim: move start, shrink length (G14 helper keeps
                      audio buffer alignment via start_in_source; MIDI clips
                      get their notes clamped). */
            wb_session_trim_clip_head(a->session, et, a->hd_track, a->hd_clip,
                                      dsmp);
            break;
        }
        case 1: {  /* RIGHT trim: change length, anchor left edge */
            wb_session_trim_clip_tail(a->session, et, a->hd_track, a->hd_clip,
                                      dsmp);
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
        case 6: {  /* G14: BODY MOVE — horizontal drag relocates the clip in
                     time; vertical drag moves it across tracks hosting the
                     same media kind. Uses wb_session_move_clip so the model
                     op is testable headless; the side-table entry travels
                     via wb_clip_edit_move. */
            double newstart = snap_pos(a, a->hd_clip_start0 + dsmp);  /* G10 */
            if (newstart < 0) newstart = 0;
            int nti = y_to_track(a, m.y);
            if (nti >= 0 && nti < (int)a->session->track_count
                && (nti == a->hd_track
                    || cl->type == a->session->tracks[nti].kind)) {
                int rc = wb_session_move_clip(a->session, a->hd_track,
                                              a->hd_clip, nti, newstart);
                if (rc == 0 && (nti != a->hd_track)) {
                    /* clip was re-appended on dst: index = count-1 */
                    int nci = (int)a->session->tracks[nti].clip_count - 1;
                    wb_clip_edit_move(et, a->hd_track, a->hd_clip, nti, nci);
                    a->hd_track = nti;
                    a->hd_clip  = nci;
                }
            }
            break;
        }
        }
        /* keep the session length covering the clip (so export/playback span it).
         * Re-read the clip: a G14 move reallocates/migrates it. */
        wb_clip *cl2 = &a->session->tracks[a->hd_track].clips[a->hd_clip];
        double end = cl2->start + cl2->length;
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
    /* G87: step-sequencer velocity drag (STEP tab) */
    if (a->vel_drag_step >= 0 && a->selected_track >= 0) {
        int dy = a->vel_drag_start_y - m.y;              /* up = louder */
        int nv = a->vel_drag_start_vel + (int)(dy * 0.8f);
        if (nv < 1) nv = 1; if (nv > 127) nv = 127;
        a->step_vel[a->selected_track][a->vel_drag_step] = nv;
        return;
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
        /* G25: write points per automation mode. WRITE always writes while
         * armed; TOUCH/LATCH write only during an active write pass. */
        wb_automation_lane *wlane = NULL;
        for (uint32_t l = 0; l < a->session->automation_count; l++)
            if (a->session->automation[l]->target == ti &&
                !strcmp(a->session->automation[l]->param, "volume"))
            { wlane = a->session->automation[l]; break; }
        int mode = wlane ? wlane->mode : 0;
        int do_write = 0;
        if (a->arm[ti] && a->fader_rec[ti]) {
            if (mode == 1) do_write = 1;                       /* WRITE */
            else if ((mode == 2 || mode == 3) && wlane->writing) do_write = 1;
            else if (mode == 0) do_write = 1;                  /* legacy arm = write */
            if ((mode == 2 || mode == 3) && !wlane->writing) {
                wlane->writing = 1;                            /* touch begins pass */
                do_write = 1;
            }
        }
        if (do_write) {
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


/* ---- G50: customizable keyboard shortcuts ------------------------------- */
/* Keymap file /tmp/bigmac_keys.txt, one "keyname=action" per line. Actions:
 * save, new, prefs, axdump. Unlisted keys keep their defaults. */
static char g_key_save[32] = "s";      /* lowercase SDLKey name */
static char g_key_new[32]  = "n";
static char g_key_prefs[32]= "p";
static char g_key_axdump[32] = "a";
static void keymap_load(void) {
    FILE *f = fopen("/tmp/bigmac_keys.txt", "r");
    if (!f) return;
    char ln[128];
    while (fgets(ln, sizeof(ln), f)) {
        char *eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0; char *val = eq + 1;
        size_t l = strlen(val);
        while (l && (val[l-1]=='\n'||val[l-1]=='\r'||val[l-1]==' ')) val[--l] = 0;
        if (!strcmp(ln, "save"))   snprintf(g_key_save, sizeof(g_key_save), "%s", val);
        if (!strcmp(ln, "new"))    snprintf(g_key_new, sizeof(g_key_new), "%s", val);
        if (!strcmp(ln, "prefs"))  snprintf(g_key_prefs, sizeof(g_key_prefs), "%s", val);
        if (!strcmp(ln, "axdump")) snprintf(g_key_axdump, sizeof(g_key_axdump), "%s", val);
    }
    fclose(f);
    printf("keymap: loaded /tmp/bigmac_keys.txt (save=%s new=%s prefs=%s)\n",
           g_key_save, g_key_new, g_key_prefs);
}
static int key_matches(const char *binding, SDL_Keycode k) {
    if (!binding[0]) return 0;
    SDL_Keycode bk = SDL_GetKeyFromName(binding);
    return bk != SDLK_UNKNOWN && bk == k;
}

/* ---- G51: preferences overlay ------------------------------------------ */
/* Shows live audio device facts (sample rate, block size, round-trip
 * latency estimate, xrun count, CPU load) plus persisted user prefs.
 * Toggle: Ctrl+P. */
static void draw_prefs(app *a) {
    SDL_Rect p = { WIN_W/2 - 220, 120, 440, 190 };
    setc(a->ren, C_PANEL); SDL_RenderFillRect(a->ren, &p);
    setc(a->ren, C_ACCENT); SDL_RenderDrawRect(a->ren, &p);
    wb_ui_draw_text(a->ren, p.x+10, p.y+8, "PREFERENCES / AUDIO", 1, C_ACCENT);
    char b[80];
    double sr = a->t.sample_rate > 0 ? a->t.sample_rate : WB_SAMPLE_RATE;
    snprintf(b, sizeof(b), "sample rate : %.0f Hz", sr);
    wb_ui_draw_text(a->ren, p.x+10, p.y+28, b, 1, C_TEXT);
    snprintf(b, sizeof(b), "block size  : %d frames", WB_MAX_BLOCK);
    wb_ui_draw_text(a->ren, p.x+10, p.y+42, b, 1, C_TEXT);
    snprintf(b, sizeof(b), "latency     : ~%.1f ms",
             (double)WB_MAX_BLOCK / sr * 1000.0);
    wb_ui_draw_text(a->ren, p.x+10, p.y+56, b, 1, C_TEXT);
    snprintf(b, sizeof(b), "xruns       : %llu",
             (unsigned long long)wb_engine_xruns(a->engine));
    wb_ui_draw_text(a->ren, p.x+10, p.y+70, b, 1,
                    wb_engine_xruns(a->engine) ? C_MUTE : C_TEXT);
    snprintf(b, sizeof(b), "cpu load    : %.1f%%",
             (double)wb_engine_cpu_load(a->engine) * 100.0);
    wb_ui_draw_text(a->ren, p.x+10, p.y+84, b, 1, C_TEXT);
    int as_secs = 120;
    const char *as_env = getenv("WB_AUTOSAVE_SECS");
    if (as_env) as_secs = atoi(as_env) > 0 ? atoi(as_env) : 120;
    snprintf(b, sizeof(b), "autosave    : every %ds (WB_AUTOSAVE_SECS)", as_secs);
    wb_ui_draw_text(a->ren, p.x+10, p.y+104, b, 1, C_TEXT_DIM);
    const char *wd = getenv("WB_WATCH_DIR");
    snprintf(b, sizeof(b), "watch dir   : %s", wd && wd[0] ? wd : "(off)");
    wb_ui_draw_text(a->ren, p.x+10, p.y+118, b, 1, C_TEXT_DIM);
    wb_ui_draw_text(a->ren, p.x+10, p.y+138,
                    "Ctrl+P closes. Env: WB_AUTOSAVE_SECS,", 1, C_TEXT_DIM);
    wb_ui_draw_text(a->ren, p.x+10, p.y+152,
                    "WB_WATCH_DIR, WB_NO_RECOVER, WB_COMMIT_STEP", 1, C_TEXT_DIM);
}

static void handle_key(app *a, SDL_Keycode k) {
    Uint32 mod = SDL_GetModState();
    int ctrl = (mod & KMOD_CTRL) != 0;
    /* G09: rename capture — printable keys append to the track name,
     * ENTER commits (disarms), ESC disarms. */
    /* G44: title text capture */
    if (a->title_entry && a->rename_armed == 0) {
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            a->title_entry = 0;
            snprintf(a->last_status, sizeof(a->last_status),
                     "TITLE SET: %.40s", a->title_text);
            printf("title: '%s' in=%.1fs out=%.1fs\n",
                   a->title_text, a->title_in, a->title_out);
            return;
        }
        if (k == SDLK_ESCAPE) { a->title_entry = 0; return; }
        size_t tl = strlen(a->title_text);
        if (k == SDLK_BACKSPACE) { if (tl) a->title_text[--tl] = 0; return; }
        if (!ctrl && k >= SDLK_SPACE && k <= SDLK_z && tl < sizeof(a->title_text)-1) {
            a->title_text[tl++] = (char)k;
            a->title_text[tl] = 0;
            return;
        }
    }
    if (a->rename_armed && a->session &&
        a->rename_track < (int)a->session->track_count) {
        wb_track *tr = &a->session->tracks[a->rename_track];
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            a->rename_armed = 0;
            printf("rename: committed '%s'\n", tr->name);
            return;
        }
        if (k == SDLK_ESCAPE) { a->rename_armed = 0; return; }
        if (k == SDLK_BACKSPACE) {
            size_t L = strlen(tr->name);
            if (L > 0) tr->name[L-1] = 0;
            return;
        }
        if (!ctrl && k >= SDLK_SPACE && k <= SDLK_z &&
            strlen(tr->name) + 1 < sizeof(tr->name)) {
            size_t L = strlen(tr->name);
            tr->name[L] = (char)k;
            tr->name[L+1] = 0;
        }
        return;   /* swallow everything else while renaming */
    }
    switch (k) {
    case SDLK_SPACE:
        if (a->t.playing) wb_engine_stop(a->engine); else wb_engine_play(a->engine);
        break;
    case SDLK_RIGHT:
        /* G15: in TRIM MODE arrows nudge the edit point frame-wise */
        if (a->trim_mode) { trim_nudge(a, WB_TRIM_FRAME); break; }
        wb_engine_seek(a->engine, a->t.song_pos + WB_SAMPLE_RATE/4);
        break;
    case SDLK_LEFT:
        if (a->trim_mode) { trim_nudge(a, -WB_TRIM_FRAME); break; }
        wb_engine_seek(a->engine, a->t.song_pos - WB_SAMPLE_RATE/4);
        break;

    /* R067: JKL shuttle — the editor's muscle memory.
     * J = reverse (speed grows each press), K = pause, L = forward.
     * Scoped to ARRANGE/EDIT tabs so PAD/STEP music keys keep working. */
    case SDLK_j:
        /* G15: JKL shuttles preview audio around the cut in TRIM MODE too */
        if (a->tab == 0 || a->tab == 1 || (a->tab == 5 && a->trim_mode)) {
            if (a->jkl_speed > 0) a->jkl_speed = -1;
            else if (a->jkl_speed > -8) a->jkl_speed--;
            else a->jkl_speed = 0;
            printf("shuttle: %d\n", a->jkl_speed);
        }
        break;
    case SDLK_l:
        if (ctrl) {  /* G35: Ctrl+L arms MIDI learn; repeat cycles the target */
            static const char *lt[] = { "MASTER VOL", "TRACK VOL", "TEMPO",
                                        "INSERT P0" };
            a->midi_learn_armed = 1;
            a->midi_learn_target = (a->midi_learn_target + 1) % 4;
            snprintf(a->last_status, sizeof(a->last_status),
                     "MIDI LEARN: move a knob for %s", lt[a->midi_learn_target]);
            printf("midilearn: armed for %s\n", lt[a->midi_learn_target]);
        } else if (a->tab == 0 || a->tab == 1 || (a->tab == 5 && a->trim_mode)) {
            if (a->jkl_speed < 0) a->jkl_speed = 1;
            else if (a->jkl_speed < 8) a->jkl_speed++;
            else a->jkl_speed = 0;
            printf("shuttle: %d\n", a->jkl_speed);
        }
        break;
    case SDLK_k:
        if (a->tab == 0 || a->tab == 1 || (a->tab == 5 && a->trim_mode)) {
            a->jkl_speed = 0;
            if (a->t.playing) wb_engine_stop(a->engine);
        } else {
            if (a->tab == 2) step_commit_to_clip(a);   /* R036 behavior */
        }
        break;
    case SDLK_o:
        if (!ctrl && a->tab == 5) {
            /* G66: cycle the global drop mode */
            a->drop_insert = a->drop_insert ? 0 : 1;
            snprintf(a->last_status, sizeof(a->last_status), "DROP: %s",
                     a->drop_insert ? "INSERT" : "OVERWRITE");
            printf("drop mode: %s\n", a->drop_insert ? "INSERT" : "OVERWRITE");
        } else if (!ctrl && (a->tab == 0 || a->tab == 1)) {
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
        if (ctrl) {  /* Ctrl+N: new (empty) project; Shift+Ctrl+N: from newest template */
            wb_session *s = NULL;
            char tmpl[512]; tmpl[0] = 0;
            if (SDL_GetModState() & KMOD_SHIFT) {
                FILE *p = popen("ls -t /tmp/bigmac_templates/*.wbus 2>/dev/null | head -1", "r");
                if (p) {
                    if (!fgets(tmpl, sizeof(tmpl), p)) tmpl[0] = 0;
                    pclose(p);
                    size_t tl = strlen(tmpl);
                    while (tl && (tmpl[tl-1]=='\n'||tmpl[tl-1]=='\r')) tmpl[--tl] = 0;
                }
                if (tmpl[0]) s = wb_session_load(tmpl);
            }
            if (!s) s = wb_session_create();
            wb_session *old = a->session;
            a->session = s;
            wb_engine_set_session(a->engine, a->session);
            a->selected_track = -1;
            snprintf(a->project_path, sizeof(a->project_path), "%s", tmpl);
            wb_session_destroy(old);
            printf("project: new %s%s\n",
                   tmpl[0] ? "session from template " : "empty session", tmpl);
        } else {
            wb_engine_set_bpm(a->engine, a->t.bpm + 1.0);
        }
        break;
    case SDLK_s:
        if (!key_matches(g_key_save, k)) break;   /* G50: remappable */
        if (ctrl && (SDL_GetModState() & KMOD_SHIFT))
            save_template(a);          /* G11: Shift+Ctrl+S = save as template */
        else if (ctrl) save_project(a, NULL);  /* Ctrl+S: save current project */
        else if (a->tab == 7) {  /* EXPORT tab: set output path */
            snprintf(a->vid_export, sizeof(a->vid_export),
                     "/tmp/bigmac_export_%d.mp4",
                     (int)(a->t.song_pos / WB_SAMPLE_RATE * 100));
            printf("video: export path set -> %s\n", a->vid_export);
        }
        break;
    case SDLK_a:
        if (ctrl && (SDL_GetModState() & KMOD_SHIFT)) {
            ax_dump();   /* G49/G60: dump accessibility map */
            snprintf(a->last_status, sizeof(a->last_status),
                     "AX MAP DUMPED to /tmp/bigmac_ax.json");
        }
        break;
    case SDLK_p:
        if (ctrl) {  /* G51: preferences overlay */
            a->prefs_visible = !a->prefs_visible;
            break;
        }
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
        if (a->trim_mode) { trim_mode_exit(a); printf("trim: EXIT\n"); }
        else if (a->razor_on) { a->razor_on = 0; printf("razor: off\n"); }
        else if (a->browser_open) a->browser_open = 0;          /* Wave1 G01 */
        else if (a->param_view) { a->param_view = 0; a->param_drag = -1; }
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
        } else if (a->tab == 5) {
            /* G16: toggle the razor; next timeline click splits ALL clips
             * under the cursor on the selected track (SHIFT = all tracks),
             * then the razor auto-exits. */
            a->razor_on = !a->razor_on;
            snprintf(a->last_status, sizeof(a->last_status), "RAZOR %s",
                     a->razor_on ? "ARMED (click timeline)" : "off");
            printf("razor: %s\n", a->razor_on ? "ARMED" : "off");
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
    case SDLK_t:  /* EDIT tab: T = G15 TRIM MODE (Ctrl+T = legacy trim start) */
        if (a->tab == 5 && a->vid_has_clip && !ctrl) {
            trim_mode_enter(a, -1);
        } else if (a->tab == 5 && a->vid_has_clip) {
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
    case SDLK_m:  /* roll cut +0.5s (Shift = -0.5s) in EDIT tab; ARRANGE tab: tap marker */
        if (a->tab == 0 && a->session) {
            wb_session_add_marker(a->session, a->t.song_pos, "M", 0);
            snprintf(a->last_status, sizeof(a->last_status), "MARKER @ %.2fs",
                     a->t.song_pos / WB_SAMPLE_RATE);
            printf("arrange: tap-marker at %.2fs\n", a->t.song_pos / WB_SAMPLE_RATE);
            break;
        }
        if (a->tab == 5 && a->vid_has_clip && a->session) {
            double d = (mod & KMOD_SHIFT) ? -0.5 : 0.5;
            int r = wb_session_roll_video_clip(a->session, a->vid_track, a->vid_clip, d);
            printf("video: roll %+.1fs -> rc=%d\n", d, r);
        }
        break;
    case SDLK_COMMA: {  /* G15 trim nudge; else ripple-trim clip start to
                         * playhead (EDIT tab) — trims the head AND shifts this
                         * + later clips back so no gap opens (Vegas "ripple
                         * trim in"). */
        if (a->tab == 5 && a->trim_mode) {
            trim_nudge(a, -WB_TRIM_FRAME);
            break;
        }
        if (a->tab == 5 && a->vid_has_clip && a->session) {
            double ph = a->t.song_pos / WB_SAMPLE_RATE;
            wb_track *tr = &a->session->tracks[a->vid_track];
            wb_clip *cl = &tr->clips[a->vid_clip];
            double cs, ce;   /* video clips: seconds */
            cs = cl->start; ce = cl->start + cl->length;
            if (ph > cs && ph < ce) {
                double delta = ph - cs;
                cl->start = ph;
                cl->length -= delta;
                /* ripple: pull later clips on this track left by delta */
                for (uint32_t c = a->vid_clip + 1; c < tr->clip_count; c++)
                    tr->clips[c].start -= delta;
                if (a->session->length > 0)
                    a->session->length -= delta * WB_SAMPLE_RATE;
                snprintf(a->last_status, sizeof(a->last_status),
                         "RIPPLE IN: -%.2fs", delta);
                printf("video: ripple trim-in %.2fs -> rc=0\n", delta);
            }
        }
        break;
    }
    case SDLK_PERIOD: {  /* G15 trim nudge; else ripple-trim clip end */
        if (a->tab == 5 && a->trim_mode) {
            trim_nudge(a, WB_TRIM_FRAME);
            break;
        }
        if (a->tab == 5 && a->vid_has_clip && a->session) {
            double ph = a->t.song_pos / WB_SAMPLE_RATE;
            wb_track *tr = &a->session->tracks[a->vid_track];
            wb_clip *cl = &tr->clips[a->vid_clip];
            double cs = cl->start, ce = cl->start + cl->length;
            if (ph > cs && ph < ce) {
                double cut = ce - ph;
                cl->length -= cut;
                for (uint32_t c = a->vid_clip + 1; c < tr->clip_count; c++)
                    tr->clips[c].start -= cut;
                if (a->session->length > 0)
                    a->session->length -= cut * WB_SAMPLE_RATE;
                snprintf(a->last_status, sizeof(a->last_status),
                         "RIPPLE OUT: -%.2fs", cut);
                printf("video: ripple trim-out %.2fs -> rc=0\n", cut);
            }
        }
        break;
    }
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
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);   /* G03: Finder drag-drop */
    app *a = calloc(1, sizeof(*a));
    {   /* G62: reduced motion (WCAG 2.3.1) via env opt-in */
        const char *rm = getenv("WB_REDUCED_MOTION");
        a->reduced_motion = rm && atoi(rm) == 1;
    }
    a->selected_track = -1;
    a->dragging_clip = -1;
    a->engine = wb_engine_create();
    a->ws = wb_workspace_create(ws_on_change, a);  /* R043: tier controller */
    a->comp_graph = wb_node_graph_create();          /* R043-G6: Fusion node view */
    keymap_load();   /* G50: customizable shortcuts */
    /* G43/G54: watch folder is opt-in via WB_WATCH_DIR */
    {
        const char *wd = getenv("WB_WATCH_DIR");
        if (wd && wd[0]) snprintf(a->watch_dir, sizeof(a->watch_dir), "%s", wd);
    }
    /* G24: keyframe track on the Gain node (node 2) for the curve editor */
    a->kf_track = wb_param_track_create();
    a->kf_drag = -1;
    if (a->kf_track && a->comp_graph) {
        wb_param_track_set(a->kf_track, 0.0, 0.4f, WB_KF_LINEAR);
        wb_param_track_set(a->kf_track, 5.0, 1.2f, WB_KF_LINEAR);
        wb_node_graph_bind_param(a->comp_graph, 2, "gain", a->kf_track);
    }
    a->cgi = wb_cgi_scene_create();                  /* R043-G7: 3D-CGI scene */
    a->agi = wb_agi_create();                        /* R043-G7: AGI task bridge */
    extern app *g_app_for_perf; g_app_for_perf = a;   /* R065 */
    a->perf = wb_perf_create(640, 360);              /* R065: performance decks */
    a->undo = wb_undo_create();                      /* Wave1 G08: undo/redo history */
    {
        /* demo decks: a red slab + a blue sphere so the PERFORMANCE
         * grid has content out of the box */
        wb_mesh *d0 = wb_mesh_box(1.2f, 1.2f, 0.25f, 255, 90, 50);
        wb_mesh *d1 = wb_mesh_sphere(1.0f, 10, 14, 60, 120, 255);
        if (d0) wb_perf_add_deck(a->perf, d0, 255, 90, 50);
        if (d1) wb_perf_add_deck(a->perf, d1, 60, 120, 255);
        wb_mesh_free(d0); wb_mesh_free(d1);
    }
    wb_agent_set_perf(a->perf);  /* R068: wire the agent bridge */
    if (file_path) {
        /* open a project from disk instead of the demo */
        a->session = wb_session_load(file_path);
        if (!a->session) { fprintf(stderr, "open: failed to load %s\n", file_path); return 1; }
        snprintf(a->project_path, sizeof(a->project_path), "%s", file_path);
        printf("open: loaded %s (%u tracks)\n", file_path, a->session->track_count);
    } else {
        /* G58: crash recovery — restore the newest autosave on relaunch.
         * WB_NO_RECOVER=1 disables (for tests / clean starts). */
        char rec[512]; rec[0] = 0;
        const char *norecov = getenv("WB_NO_RECOVER");
        if (!norecov || atoi(norecov) == 0) {
            static const char *cmd_fmt =
                "ls -t /tmp/bigmac_autosave/*.wbus 2>/dev/null | head -1";
            FILE *p = popen(cmd_fmt, "r");
            if (p) {
                if (!fgets(rec, sizeof(rec), p)) rec[0] = 0;
                pclose(p);
                /* trim newline */
                size_t rl = strlen(rec);
                while (rl > 0 && (rec[rl-1] == '\n' || rec[rl-1] == '\r'))
                    rec[--rl] = 0;
            }
        }
        if (rec[0]) {
            a->session = wb_session_load(rec);
            if (a->session) {
                snprintf(a->project_path, sizeof(a->project_path), "%s", rec);
                printf("recover: restored autosave %s (%u tracks)\n",
                       rec, a->session->track_count);
            } else {
                fprintf(stderr, "recover: autosave %s unreadable; demo instead\n", rec);
            }
        }
        if (!a->session) {
            a->session = wb_session_demo();
            a->project_path[0] = 0;
        }
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
    a->scale_lock   = 0;   /* G80: scale-lock off by default */
    a->chord_mode   = 0;   /* G81: chord stamp off by default */
    a->step_sel     = -1;  /* G87: no step selected for velocity edit */
    a->vel_drag_step= -1;  /* G87: no active step velocity drag */
    /* G87/G88: per-step velocity defaults to 100, probability to 100% */
    for (int t = 0; t < WB_MAX_TRACKS; t++)
        for (int s = 0; s < 16; s++) {
            a->step_vel[t][s]  = 100;
            a->step_prob[t][s] = 100;
        }
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
            else if (ev.type==SDL_DROPFILE) {   /* G03: Finder -> timeline */
                char *dp = ev.drop.file;
                if (dp && dp[0]) {
                    browser_import(a, dp);
                }
                if (dp) SDL_free(dp);
            }
            else if (ev.type==SDL_MOUSEBUTTONUP) {
                /* G25: TOUCH mode ends its write pass on finger-off */
                if (a->dragging_fader >= 0 && a->session &&
                    a->dragging_fader < (int)a->session->track_count) {
                    int rti = a->dragging_fader;
                    for (uint32_t l = 0; l < a->session->automation_count; l++) {
                        wb_automation_lane *rl = a->session->automation[l];
                        if (rl->target == rti &&
                            !strcmp(rl->param, "volume") &&
                            rl->mode == 2)
                            rl->writing = 0;   /* LATCH intentionally persists */
                    }
                }
                a->param_drag = -1; a->vel_drag_track = -1; a->ov_drag = 0; a->cgi_dragging = 0; a->handle_drag = -1; a->vel_drag_step = -1; a->dragging_fader = -1; a->kf_drag = -1;
            }
        }
        render(a);
        perf_tick(a);
        batch_tick(a);   /* G53: pump the batch render matrix */
        if (a->prefs_visible) draw_prefs(a);
        watch_poll(a);   /* G43/G54: watch-folder auto-import/auto-render */
        /* G57: autosave — every 120s, to a dated Auto-Save folder (Premiere
         * convention). Only when a project path exists OR the session has
         * content; atomic via wb_session_save's own write. Keeps last 5. */
        {
            time_t now = time(NULL);
            /* G57: WB_AUTOSAVE_SECS overrides the 120s default (tests). */
            const char *as_env = getenv("WB_AUTOSAVE_SECS");
            long as_secs = as_env ? atol(as_env) : 120;
            if (a->session && as_secs > 0 && now - a->last_autosave >= as_secs) {
                const char *dir = "/tmp/bigmac_autosave";
                mkdir(dir, 0755);
                /* prune: keep only the 5 newest autosaves */
                {
                    char cmd[512];
                    snprintf(cmd, sizeof cmd,
                        "ls -t %s/*.wbus 2>/dev/null | tail -n +6 | xargs rm -f 2>/dev/null",
                        dir);
                    int prc = system(cmd); (void)prc;
                }
                char path[512];
                snprintf(path, sizeof path, "%s/auto_%lld.wbus", dir, (long long)now);
                if (wb_session_save(a->session, path) == 0)
                    snprintf(a->last_status, sizeof a->last_status,
                             "AUTOSAVED %.80s", path);
                a->last_autosave = now;
            }
        }
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
    if (a->undo) wb_undo_destroy(a->undo);   /* Wave1 G08 */
    wb_engine_destroy(a->engine); wb_session_destroy(a->session);
    free(a); SDL_Quit();
    return 0;
}
