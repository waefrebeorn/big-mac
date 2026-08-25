/* wbus_compositor.h — pull-based, RoI/RoD node compositor (R013 D1/D3).
 *
 * Design (converged from Natron/Olive/VapourSynth/AVISynth research):
 *   - ONE recursive call: pull(node, t, roi) -> frame
 *   - each node clips the requested RoI to its own RoD (region of definition)
 *   - isIdentity short-circuits no-ops (VirtualDub-style bypass)
 *   - a cache node auto-inserted at graph edges memoizes by content hash
 *     (Hash64(time, roi, node-id)) with BOUNDED LRU eviction
 *   - two-phase: request inputs (phase 0), compute when ready (phase 1)
 *
 * RGBA float frames. Intentionally small + C11; the same contract scales
 * to OFX plugin nodes later (RoI/tile + identity skip already match OFX).
 */

#ifndef WUBUS_WBUS_COMPOSITOR_H
#define WUBUS_WBUS_COMPOSITOR_H


#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stddef.h>

/* RGBA float pixel */
typedef struct { float r, g, b, a; } wb_px;

/* A frame: w*h RGBA. roi = region of interest actually filled.
 * `gpu` marks the pixel-buffer ownership boundary (G12): 0 = CPU-owned
 * (authoritative), 1 = eligible for GPU interop (a Metal layer may wrap the
 * `px` buffer; CPU path stays the source of truth). */
typedef struct wb_frame {
    int w, h;
    int roi_x, roi_y, roi_w, roi_h;  /* valid sub-rect */
    wb_px *px;                        /* w*h pixels */
    int gpu;                          /* G12 backend-ownership flag */
} wb_frame;

/* G12: render backend (the offload boundary). CPU is authoritative today;
 * GPU is the future Metal-interop slot — pixel buffers are swappable so a
 * GPU tile can wrap `wb_px` without changing the node contract. */
typedef enum {
    WB_RENDER_CPU = 0,
    WB_RENDER_GPU
} wb_render_backend;

typedef enum {
    WB_NODE_SOURCE = 0,   /* wraps a producer (e.g. decoded clip) */
    WB_NODE_EFFECT,       /* applies an op to its input(s) */
    WB_NODE_CACHE,        /* auto-inserted memoization wrapper */
    WB_NODE_COMPOSITE,    /* blends inputs by alpha (layer stack) */
    WB_NODE_COLORSPACE,   /* R018-B: color-space / transfer transform (CST) */
    WB_NODE_TONEMAP       /* R018-B: HDR->SDR tone map (Reinhard/hable) */
} wb_node_kind;

typedef struct wb_node wb_node;

/* callbacks the host supplies */
typedef wb_frame* (*wb_node_pull_fn)(wb_node *self, double t,
                                     int rx, int ry, int rw, int rh, int phase);
typedef void (*wb_node_free_fn)(wb_node *self);

#include "wbus/wbus_param_track.h"
#include "wbus/wbus.h"   /* wb_automation_lane bridge (G11 unified bus) */

struct wb_node {
    wb_node_kind kind;
    char id[32];
    int   n_inputs;
    wb_node **inputs;       /* up to N inputs */
    void  *user;            /* node-specific state */
    wb_node_pull_fn pull;
    wb_node_free_fn free;
    /* identity optimization: set during request-phase to skip compute */
    int is_identity;
    /* R074 fix: declared output format (0 = infer from inputs). Sources
     * set it at creation; pulls clip against it instead of a hardcoded
     * max. Fixes the resolution family of bugs. */
    int fmt_w, fmt_h;
    /* G11: named keyframed parameters on this node (the shared bus).
     * The compositor pulls param tracks per-frame so FX params animate.
     * A param slot is fed by EITHER a keyframed track OR a session
     * automation lane (both are the same unified bus; track overrides lane). */
    wb_param_track **params;
    wb_automation_lane **param_lanes;
    int   n_params;
    char  param_names[16][32];
};

wb_frame *wb_frame_alloc(int w, int h);
void      wb_frame_free(wb_frame *f);
/* clip roi to (0,0,w,h); returns 0 if roi is empty */
int  wb_roi_clip(int w, int h, int *rx, int *ry, int *rw, int *rh);

/* generic pull entry: handles identity short-circuit + forwarding. */
wb_frame *wb_node_pull(wb_node *n, double t, int rx, int ry, int rw, int rh);

/* ---- G11 param bus: bind a keyframed track to a named node param ------ */
/* Returns the param slot index (>=0), or -1. Name stored for debugging. */
int  wb_node_add_param(wb_node *n, const char *name, wb_param_track *tr);

/* G11 unified bus: bind a session automation LANE directly to a node param.
 * The node reads wb_automation_value_at(lane, t) each pull, so recorded
 * audio automation and video-FX params ride the SAME channel. Precedence:
 * a keyframed track param (if present) overrides the lane. */
int  wb_node_add_param_lane(wb_node *n, const char *name, wb_automation_lane *lane);

/* Get the animated value of a node param at time t (0 if unset). */
float wb_node_param_value(const wb_node *n, const char *name, double t);

/* G67: basic color grading — Lift/Gamma/Gain applied to a frame in place.
 * lift/gain are additive/multiplicative offsets (-1..1 typical), gamma is
 * the classic exponent (1.0 = neutral, <1 brightens mids, >1 darkens).
 * exposure (stops-ish linear) and saturation multiplier ride along since
 * the clip model already carries them (R018-C). All clamped to [0,1]. */
void wb_frame_grade(wb_frame *f, float lift, float gamma, float gain,
                    float exposure, float saturation);

/* G12: GPU-offload boundary. The CPU path is always authoritative; the
 * backend flag marks where a future Metal interop layer slots in. Frames
 * carry a `gpu` ownership flag for the swap boundary. */
void wb_compositor_set_backend(wb_render_backend b);
wb_render_backend wb_compositor_get_backend(void);
void wb_frame_set_gpu(wb_frame *f, int gpu);
int  wb_frame_get_gpu(const wb_frame *f);

/* G1: proxy-scale / quality-of-service dial (0.0 = draft/proxy, 1.0 = full).
 * Drives tile size and proxy-vs-fullres swaps on slow frames (R017 G1,
 * modeled on GStreamer QoS + Resolve render cache). Stored process-globally
 * so any node pull can consult it. */
void wb_compositor_set_quality(double q);   /* 0..1 */
double wb_compositor_get_quality(void);
/* Effective tile size for the current quality (smaller tiles = cheaper
 * per-pull work on a dual-core machine under load). */
int  wb_compositor_tile_size(void);

/* G3: two-phase pull. wb_node_pull is the compute (phase 1) entry;
 * wb_node_pull_request issues a request/prepare pass (phase 0) so slow
 * decoders can run ahead before the compute pass. VapourSynth-style
 * arInitial -> arAllFramesReady. */
void wb_node_pull_request(wb_node *n, double t, int rx, int ry, int rw, int rh);

/* G3: a decode source modeling an async (expensive) frame decoder.
 * phase 0 schedules the decode; phase 1 completes + returns the frame. */
wb_node *wb_node_decode_source(float r, float g, float b, float a, int w, int h);
int wb_node_decode_is_requested(const wb_node *n);
int wb_node_decode_is_ready(const wb_node *n);

/* G2: auto-insert a bounded LRU cache after every non-source node in the
 * graph (AVISynth internal caching / Natron per-node hash cache). Returns
 * the number of caches inserted (idempotent). */
int wb_graph_auto_cache(wb_node *root, int max_frames);
wb_node_kind wb_node_get_kind(const wb_node *n);
/* Report cache occupancy/hits (G2 verification). */
int wb_node_cache_stats(const wb_node *n, int *hits, int *count);

/* ---- convenience node factories ------------------------------------- */
/* SOURCE: returns a solid color or a (future) decoded producer.
 * color producer for now (deterministic, testable). */
wb_node *wb_node_source_color(float r, float g, float b, float a, int w, int h);

/* R073 hop 43: CGI source — renders a wb_anim 3D scene at time t into the
 * node graph (Blender-lite compositing input). The anim is NOT owned. */
struct wb_anim;
wb_node *wb_node_source_anim(struct wb_anim *anim, int w, int h);

/* R074 hop 113 (G-SF030/031): SNES ordered-dither + palette quantize. */
wb_node *wb_node_effect_dither(int levels);
/* R074 hop 116 (#10): bilinear scaler — resize to out_w x out_h. */
wb_node *wb_node_effect_scaler(int out_w, int out_h);
/* R074 hop 119 (G-SF051/052/053): presentation effects. */
wb_node *wb_node_effect_letterbox(float bar_fraction);
wb_node *wb_node_effect_scanline(float strength);
wb_node *wb_node_effect_chromatic(float offset_px);
/* #78: resolve a node's output dimensions without pulling. */
int wb_node_output_dims(wb_node *n, int *w, int *h);
/* G-SF032: Mode-7 affine ground warp. */
wb_node *wb_node_effect_mode7(float horizon_frac, float strength,
                              double scroll_speed);

/* R074 hop 111: scene source — smoothstep gradient (vertical or radial)
 * with a moving light sweep. Colors are top/bottom endpoints. */
wb_node *wb_node_source_scene(float r0, float g0, float b0,
                              float r1, float g1, float b1,
                              int mode, float band_speed,
                              int w, int h);

/* R074 hop 112: frame source — serve an external RGBA buffer (caller
 * updates the buffer between pulls; the node does not copy on create). */
wb_node *wb_node_source_frame(int w, int h, uint8_t *rgba);

/* R073 hop 44: title/text generator node (built-in 5x7 font). */
wb_node *wb_node_source_text(const char *text, int scale,
                             float r, float g, float b, float a,
                             int w, int h);
/* R073 hop 58: set the text animation preset on a text source node. */
void wb_node_source_text_anim(wb_node *text_node, int mode, double dur);
/* R074 hop 113: position a text node in pixels. */
void wb_node_source_text_pos(wb_node *text_node, int x, int y);

/* R073 hop 49: transitions — op 0 = crossfade, op 1 = dip-to-black.
 * Two inputs (A, B); mixes across `duration_secs` starting at t=0.
 * Attach inputs with wb_transition_add(A then B). */
wb_node *wb_node_transition(int op, double duration_secs);
void      wb_transition_add(wb_node *trans, wb_node *child);
/* R074 fix: declare/query a node's output format. Set on sources (or any
 * node) to pin resolution; effects/transitions infer from first input.
 * Returns 0 and fills w/h, or -1 if unknown. */
int  wb_node_set_format(wb_node *n, int w, int h);
int  wb_node_get_format(const wb_node *n, int *w, int *h);

/* R073 hop 101: write a frame as binary PPM (P6). Returns 0 or -1. */
int wb_frame_write_ppm(const wb_frame *f, const char *path);
/* R073 hop 104: render a transition graph to an mp4 (H.264) file. */
int wb_compositor_export_mp4(wb_node *trans, const char *mp4_path,
                             double dur, int fps, int w, int h);
/* R073 hop 105: same, with a WAV muxed as AAC audio track. */
int wb_compositor_export_mp4_audio(wb_node *trans, const char *mp4_path,
                                   const char *wav_path,
                                   double dur, int fps, int w, int h);

/* R073 hop 96: transition style presets — one call builds a configured
 * transition. 0=MusicVideo (crossfade, tight feather), 1=News (fast
 * linear wipe), 2=Cinematic (long dip-to-black), 3=VJ (zoom-blur). */
wb_node *wb_transition_preset(int preset, double duration_secs);
/* R073 hop 64: wipe direction — 0 forward, 1 reversed. */
void      wb_transition_dir(wb_node *trans, int dir);

/* EFFECT: applies a simple op over its single input.
 * op: 0 = identity(bypass), 1 = brightness*gain (gain is keyframable
 *      via param "gain"), 2 = invert-alpha matte */
wb_node *wb_node_effect(int op, float gain);

/* TRANSFORM: affine scale/pan/rotate over its single input (Ken Burns /
 * zoom-punch). Params "scale" (1=100%), "cx","cy" (normalized pivot 0..1),
 * "rot" (radians) are keyframable via the G11 param bus, so a clip can
 * animate (e.g. slow zoom-in) without re-encoding. */
wb_node *wb_node_transform(void);

/* COMPOSITE: blends up to 8 inputs (bottom..top) by alpha (over operator). */
wb_node *wb_node_composite(void);

/* CACHE: wraps a child node; memoizes by hash(time,roi,child-id),
 * bounded LRU (max_frames). */
wb_node *wb_node_cache(wb_node *child, int max_frames);

/* R018-B: COLORSPACE / transfer transform (the two-step CST of Resolve).
 * mode selects the transform applied per-pixel in linear-ish space:
 *   WB_CS_SRGB_TO_LINEAR   decode sRGB/Rec.709 gamma -> linear
 *   WB_CS_LINEAR_TO_SRGB   encode linear -> sRGB/Rec.709 gamma
 *   WB_CS_PQ_TO_LINEAR     HDR10 ST.2084 (PQ) decode -> linear (10000 nit)
 *   WB_CS_LINEAR_TO_PQ     linear -> HDR10 PQ encode
 *   WB_CS_HLG_TO_LINEAR    HLG (ARIB STD-B67) decode -> linear
 *   WB_CS_LINEAR_TO_HLG    linear -> HLG encode
 *   WB_CS_REC709_TO_2020   wide-gamut matrix Rec.709 -> Rec.2020
 *   WB_CS_REC2020_TO_709   wide-gamut matrix Rec.2020 -> Rec.709 */
typedef enum {
    WB_CS_SRGB_TO_LINEAR = 0, WB_CS_LINEAR_TO_SRGB,
    WB_CS_PQ_TO_LINEAR, WB_CS_LINEAR_TO_PQ,
    WB_CS_HLG_TO_LINEAR, WB_CS_LINEAR_TO_HLG,
    WB_CS_REC709_TO_2020, WB_CS_REC2020_TO_709
} wb_cs_mode;
wb_node *wb_node_colorspace(wb_cs_mode mode);

/* R018-B: HDR -> SDR tone map (operates in linear light).
 *   WB_TM_NONE     passthrough (clamp)
 *   WB_TM_REINHARD Reinhard: c/(1+c) (film-like, preserves highlights softly)
 *   WB_TM_ACES     ACES filmic (Narkowicz) — monotonic, bounded [0,1) */
typedef enum { WB_TM_NONE = 0, WB_TM_REINHARD, WB_TM_ACES } wb_tm_op;
wb_node *wb_node_tonemap(wb_tm_op op);

void wb_node_destroy(wb_node *n);

/* attach an input to a composite (caller keeps ownership of child) */
void wb_composite_add(wb_node *comp, wb_node *child);

/* R043 (G6): self-contained Fusion-style node-graph view model.
 * Owns a demo compositing chain (Source -> Effect -> Composite -> Output)
 * plus per-node 2D layout so the UI can render a node graph WITHOUT the
 * UI knowing the node internals. The graph is opaque; the UI iterates via
 * the accessors below. */
typedef struct wb_node_graph wb_node_graph;

wb_node_graph *wb_node_graph_create(void);   /* builds the demo chain + layout */
void            wb_node_graph_destroy(wb_node_graph *g);
int             wb_node_graph_count(const wb_node_graph *g);
/* per-node accessors for drawing (no node internals leaked to the UI) */
const char     *wb_node_graph_label(const wb_node_graph *g, int i);
wb_node_kind    wb_node_graph_kind(const wb_node_graph *g, int i);
int             wb_node_graph_inputs(const wb_node_graph *g, int i);  /* # inputs */
int             wb_node_graph_input_of(const wb_node_graph *g, int i, int k); /* node idx feeding input k */
void            wb_node_graph_pos(const wb_node_graph *g, int i, float *x, float *y);
float           wb_node_graph_param(const wb_node_graph *g, int i, double t); /* animated param preview */
/* G24: direct node access for the keyframe graph editor (do not destroy). */
struct wb_node *wb_node_graph_node_at(const wb_node_graph *g, int i);
/* G24: bind a param track to a graph node by index (graph does NOT own it —
 * caller keeps ownership). Returns slot idx or -1. */
int             wb_node_graph_bind_param(const wb_node_graph *g, int i,
                                         const char *name,
                                         wb_param_track *tr);

/* R073 hop 82: drive a power window's center along a keyframed path. */
int wb_node_window_track_path(wb_node *win_effect,
                              const double *ts, const float xs[],
                              const float ys[], int n, double dur);

#ifdef __cplusplus
}
#endif
#endif /* WUBUS_WBUS_COMPOSITOR_H */
