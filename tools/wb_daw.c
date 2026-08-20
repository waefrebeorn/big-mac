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
#include "wbus_captions.h"
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
        "KEYS", "PAD", "STEP", "SESSION",
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

    /* R023: velocity-drag — when dragging a note vertically, change its velocity */
    int vel_drag_track;      /* -1 = none, else track index */
    int vel_drag_clip;       /* clip index */
    int vel_drag_note;       /* note index */
    int vel_drag_start_y;    /* mouse y where drag began */
    int vel_drag_start_vel;  /* velocity at drag start */

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
    SDL_Texture *vid_preview_tex; /* cached preview frame */

    /* tool paths */
    char ffmpeg_path[256];
    char whisper_cli_path[256];
    char whisper_model_path[256];

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

    /* bar:beat (convert samples -> seconds first!) */
    double sec_bb = a->t.song_pos / (double)WB_SAMPLE_RATE;
    double bps = a->t.bpm / 60.0;
    double beat = sec_bb * bps;
    int bar = (int)(beat / 4) + 1;
    int bi  = ((int)beat % 4) + 1;
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

    /* R022: arrangement-marker ruler — song-section labels (Intro/Verse/..) */
    if (a->session->marker_count > 0) {
        int my = TRANSPORT_H + RULER_H - 12;
        for (uint32_t mi = 0; mi < a->session->marker_count; mi++) {
            const wb_marker *mk = &a->session->markers[mi];
            int mx = arr_x(a, mk->pos);
            if (mx < GUTTER_W) continue;
            setc(a->ren, mk->kind ? C_ACCENT : C_SOLO);   /* section vs cue */
            SDL_Rect band = { mx, my, 3, RULER_H };
            SDL_RenderFillRect(a->ren, &band);
            wb_ui_draw_text(a->ren, mx + 4, TRANSPORT_H + 2, mk->label, 1,
                            mk->kind ? C_ACCENT : C_SOLO);
        }
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
                /* pitch maps to vertical: full lane = PITCH_SPAN semitones, low at bottom */
                int span = 24;  /* 2 octaves visible per lane */
                int row = nt->pitch % span;
                int cell_h = th / span;
                int ny = y + th - (row+1)*cell_h;
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

    /* R023: velocity lane — a strip at the bottom of the arrangement showing
     * each MIDI note's velocity as a vertical bar (Ableton/Logic style). */
    {
        int vy = TRANSPORT_H + RULER_H + ARRANG_H - 34;
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

/* forward declarations for video editor tab functions (defined after render) */
static void draw_tab_bar(app *a);
static void draw_video_preview(app *a);
static void draw_video_timeline(app *a);
static void draw_video_tab_panel(app *a);

static void render(app *a) {
    wb_engine_get_transport(a->engine, &a->t);
    setc(a->ren, C_BG); SDL_RenderClear(a->ren);
    draw_transport(a);
    if (a->tab >= 4 && a->tab <= 7) {
        draw_tab_bar(a);
        draw_video_preview(a);
        draw_video_timeline(a);
        draw_video_tab_panel(a);
    } else {
        draw_tab_bar(a);
        draw_ruler(a);
        draw_arrangement(a);
        draw_mixer(a);
        draw_param_editor(a);
    }
    SDL_RenderPresent(a->ren);
}

/* ---- video editor functions (DaVinci-style tabs) ---- */

static void draw_tab_bar(app *a) {
    int bar_y = TRANSPORT_H + RULER_H;
    int tab_w = (WIN_W - MIXER_W) / 8;
    for (int i = 0; i < 8; i++) {
        int x = GUTTER_W + i * tab_w;
        int active = (i == a->tab);
        setc(a->ren, active ? C_ACCENT : C_PANEL2);
        SDL_Rect tb = { x, bar_y, tab_w - 2, 22 };
        SDL_RenderFillRect(a->ren, &tb);
        setc(a->ren, active ? C_BG : C_TEXT_DIM);
        wb_ui_draw_text(a->ren, x + 4, bar_y + 4, tab_name(i), 1, active ? C_BG : C_TEXT_DIM);
    }
    setc(a->ren, C_BG);
    SDL_Rect sep = { GUTTER_W, bar_y + 23, WIN_W - MIXER_W - GUTTER_W, 1 };
    SDL_RenderFillRect(a->ren, &sep);
}

static SDL_Rect video_preview_rect(app *a) {
    SDL_Rect r;
    r.x = GUTTER_W;
    r.y = TRANSPORT_H + RULER_H + 26;
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
    wb_video_decoder *vd = wb_video_decoder_open(a->vid_source);
    if (vd) {
        uint8_t *rgba = calloc(PROXY_SCALE_W * PROXY_SCALE_H, 4);
        int out_w = PROXY_SCALE_W, out_h = PROXY_SCALE_H;
        if (wb_video_decoder_seek(vd, clip_time) == 0 &&
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
    int py = TRANSPORT_H + RULER_H + 26;
    int pw = MIXER_W - 8;
    int ph = WIN_H - TRANSPORT_H - RULER_H - 26;
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
        snprintf(buf, sizeof(buf), "Clip editor");
        wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 20;
        if (a->vid_has_clip) {
            snprintf(buf, sizeof(buf), "Source: %s", a->vid_source);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT_DIM); yy += 16;
            snprintf(buf, sizeof(buf), "Duration: %.2f s", a->vid_dur);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 14;
            snprintf(buf, sizeof(buf), "In: %.2f s  Out: %.2f s", a->vid_tl_start, a->vid_tl_end);
            wb_ui_draw_text(a->ren, px + 6, yy, buf, 1, C_TEXT); yy += 20;
            wb_ui_draw_text(a->ren, px + 6, yy, "Edit shortcuts:", 1, C_TEXT); yy += 16;
            wb_ui_draw_text(a->ren, px + 6, yy, "^T trim start  ^E trim end", 1, C_TEXT_DIM); yy += 14;
            wb_ui_draw_text(a->ren, px + 6, yy, "^X split  ^D delete", 1, C_TEXT_DIM);
        } else {
            wb_ui_draw_text(a->ren, px + 6, yy, "No clip. Import a video first.", 1, C_TEXT_DIM);
        }
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
        wb_ui_draw_text(a->ren, px + 6, yy, "Shortcuts:", 1, C_TEXT); yy += 16;
        wb_ui_draw_text(a->ren, px + 6, yy, "^G generate  ^B burn", 1, C_TEXT_DIM);
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
    snprintf(proxy_path, sizeof(proxy_path), "/tmp/bigmac_proxy_%d.mp4", (int)(a->vid_dur * 100));
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
        snprintf(tr->name, sizeof(tr->name), "Video");
        a->session->track_count++;
    }
    int ci = wb_session_add_video_clip(a->session, vt, path, 0.0);
    if (ci < 0) { fprintf(stderr, "video: failed to add clip\n"); return -1; }
    wb_session_set_video_proxy(a->session, vt, ci, proxy_path);
    a->vid_track = vt;
    a->vid_clip = ci;
    a->vid_tl_start = 0.0;
    a->vid_tl_end = a->vid_dur;
    a->vid_has_clip = 1;
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
            int top = TRANSPORT_H + RULER_H;
            int ny = top + (int)(ti*track_h) + (int)track_h - (row+1)*cell_h;
            if (x >= nx && x <= nx+nw && y >= ny && y <= ny+cell_h) {
                a->vel_drag_clip = (int)c; a->vel_drag_note = (int)k;
                return (int)c; /* caller also has note idx via a->vel_drag_note */
            }
        }
    }
    return -1;
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
        /* piano-roll: left-click on an existing note starts a VELOCITY drag
         * (R023); left-click on empty lane adds a note (1 beat, mid vel) */
        a->vel_drag_track = -1;
        if (b.y > TRANSPORT_H + RULER_H) {
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
    /* R023: velocity drag — vertical mouse motion on a held note changes its
     * velocity (like dragging up/down on a note in Ableton/Logic). */
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
    case SDLK_l:
        /* cycle scale root down */
        a->scale_root = (a->scale_root + 11) % 12;
        a->param_drag = -1;
        printf("scale: %s (root %d)\n", scale_name(a->scale_root, a->scale_type), a->scale_root);
        break;
    case SDLK_i:
        if (a->tab >= 4 && a->tab <= 7) {
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
                    printf("captions: generated %s\n", srt_path);
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
    default: break;
    }
}

int main(int argc, char **argv) {
    int shot = 0;
    wb_backend *audio = NULL;
    const char *shot_path = NULL;
    const char *file_path = NULL;
    int forced_view = -1;
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"--screenshot")==0) { shot=1; shot_path = (i+1<argc)?argv[i+1]:"/tmp/wbdaw.ppm"; }
        else if (strcmp(argv[i],"--view")==0 && i+1<argc) { forced_view = atoi(argv[i+1]); }
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

    /* tabbed view default: keyboard piano roll, A minor scale */
    a->tab          = (forced_view >= 0 && forced_view <= 7) ? forced_view : 0;   /* KEYS or forced */
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
        /* for video-editor views, set up a demo clip so the panels show
         * real content (source + proxy + timeline), not an empty state. */
        if (forced_view >= 4 && forced_view <= 7) {
            const char *demo = "/tmp/bigmac_demo_src.mp4";
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
            if (access(demo, F_OK) == 0) video_import(a, demo);
        }
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
            else if (ev.type==SDL_MOUSEBUTTONUP) { a->param_drag = -1; a->vel_drag_track = -1; }
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
