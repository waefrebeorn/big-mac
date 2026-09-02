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
    int refs;                         /* R074 hop 144 (#47/#82) */
} wb_frame;
/* Take a shared reference (frame is freed when the last ref drops).
 * Returns the same pointer for chaining. */
wb_frame *wb_frame_ref(wb_frame *f);

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
    int     owns_params;   /* #71/#80: node frees its tracks */
    wb_automation_lane **param_lanes;
    int   n_params;
    char  param_names[16][32];
};

wb_frame *wb_frame_alloc(int w, int h);
void      wb_frame_free(wb_frame *f);
/* Internal: create a node (used by external node-type modules). */
wb_node *wb_node_create(wb_node_kind kind, const char *id);
/* clip roi to (0,0,w,h); returns 0 if roi is empty */
int  wb_roi_clip(int w, int h, int *rx, int *ry, int *rw, int *rh);

/* generic pull entry: handles identity short-circuit + forwarding. */
wb_frame *wb_node_pull(wb_node *n, double t, int rx, int ry, int rw, int rh);
/* #48: tile-based pull — renders in tile_size chunks and stitches.
 * Falls back to a single pull when the ROI already fits one tile. */
wb_frame *wb_node_pull_tiled(wb_node *n, double t, int rx, int ry,
                             int rw, int rh);

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
/* R077: VFX effect nodes — wrap wb_vfx.c as compositor nodes.
 * Each takes a keyframable param (bind via wb_node_add_param) and converts
 * float<->uint8 internally since wb_vfx operates on uint8_t RGBA. */
wb_node *wb_node_effect_deep_fry(void);
wb_node *wb_node_effect_vhs(void);
wb_node *wb_node_effect_rgb_glitch(void);
wb_node *wb_node_effect_posterize(void);
wb_node *wb_node_effect_vignette(void);
wb_node *wb_node_effect_vfx_chromatic(void);
wb_node *wb_node_effect_camera_shake(void);

/* R084: audio-reactive video FX — reads audio features (bass/beat/energy)
 * and applies flash, shake, zoom, color shift to video. */
wb_node *wb_node_effect_audio_reactive(float intensity);
void wb_audio_reactive_node_set_params(float zoom, float flash, float shake,
                                        float color_shift, float brightness);
void wb_audio_reactive_node_get_params(float *zoom, float *flash, float *shake,
                                        float *color_shift, float *brightness);

/* R085: professional video FX nodes */
wb_node *wb_node_effect_stabilize(void);
void wb_node_effect_stabilize_set_enabled(wb_node *n, int enabled);
wb_node *wb_node_effect_chromakey(float r, float g, float b, float threshold);
void wb_node_effect_chromakey_set_color(wb_node *n, float r, float g, float b);
wb_node *wb_node_effect_transform_pro(void);
void wb_node_effect_transform_pro_set_pos(wb_node *n, float x, float y);
void wb_node_effect_transform_pro_set_scale(wb_node *n, float s);
void wb_node_effect_transform_pro_set_rotation(wb_node *n, float radians);

/* R086: mesh warp / puppet tool — deformable grid for character animation.
 * Divides input into grid_w x grid_h cells; vertices can be pinned to
 * arbitrary positions. Surrounding mesh follows pins based on stiffness
 * (0.0 = rigid, 1.0 = full propagation). Bilinear interpolation within
 * each cell gives smooth deformation (AE Puppet Tool parity). */
wb_node *wb_node_effect_mesh_warp(int grid_w, int grid_h);
void wb_node_effect_mesh_warp_set_pin(wb_node *n, int grid_x, int grid_y,
                                       float pin_x, float pin_y);
void wb_node_effect_mesh_warp_set_stiffness(wb_node *n, float stiffness);
void wb_node_effect_mesh_warp_clear_pins(wb_node *n);

/* R085: 3D LUT color grading node */
wb_node *wb_node_effect_lut(const char *path);
void wb_node_effect_lut_set_intensity(wb_node *n, float intensity);
int wb_node_effect_lut_load(wb_node *n, const char *path);
/* #78: resolve a node's output dimensions without pulling. */
int wb_node_output_dims(wb_node *n, int *w, int *h);
/* G-SF080 v3: wire src into dst's input slot k (grows input count). */
int wb_node_connect(wb_node *dst, wb_node *src, int k);
/* G-SF042: depth-aware merge of two same-size frames. */
void wb_comp_zmerge(wb_frame *a, const wb_frame *b,
                    const float *da, const float *db);
/* G-SF032: Mode-7 affine ground warp. */
wb_node *wb_node_effect_mode7(float horizon_frac, float strength,
                              double scroll_speed);

/* R085: motion tracking overlay node */
wb_node *wb_node_effect_motion_track(void);
int wb_node_effect_motion_get_points(const wb_node *n, float *xs, float *ys,
                                      int max_points);

/* R086: directional motion blur — AE-style shutter model.
 * samples: sub-samples per frame (default 8). More = smoother but slower.
 * Transform-driven: tracks pos_x/pos_y/scale/rotation params across frames
 * and accumulates sub-samples along the motion vector. */
wb_node *wb_node_effect_motion_blur(int samples);
void wb_node_effect_motion_blur_set_shutter_angle(wb_node *n, float angle);
void wb_node_effect_motion_blur_set_shutter_phase(wb_node *n, float phase);

/* R074 hop 111: scene source — smoothstep gradient (vertical or radial)
 * with a moving light sweep. Colors are top/bottom endpoints. */
wb_node *wb_node_source_scene(float r0, float g0, float b0,
                              float r1, float g1, float b1,
                              int mode, float band_speed,
                              int w, int h);

/* R074 hop 112: frame source — serve an external RGBA buffer (caller
 * updates the buffer between pulls; the node does not copy on create). */
wb_node *wb_node_source_frame(int w, int h, uint8_t *rgba);

/* VIDEO source node — wraps wb_video_decoder for pull-based compositing.
 * Opens the file, seeks + decodes one frame per pull, converts uint8 RGBA
 * to float wb_px. Output dimensions default to PROXY_SCALE_W/H (854x480). */
wb_node *wb_node_source_video(const char *path, int proxy_w, int proxy_h);
double   wb_node_source_video_duration(const wb_node *n);
int      wb_node_source_video_width(const wb_node *n);
int      wb_node_source_video_height(const wb_node *n);

/* R073 hop 44: title/text generator node (built-in 5x7 font). */
wb_node *wb_node_source_text(const char *text, int scale,
                             float r, float g, float b, float a,
                             int w, int h);
/* R073 hop 58: set the text animation preset on a text source node. */
void wb_node_source_text_anim(wb_node *text_node, int mode, double dur);

/* ---- Per-character text animator (AE parity) ----------------------- */
/* Effect node: animates text one character at a time with stagger,
 * easing, and selector ranges. Uses wb_ui_font rasterizer. */
wb_node *wb_node_effect_text_animator(const char *text, int scale,
                                       float r, float g, float b, float a,
                                       int w, int h);
/* Select character range [start_char, end_char) to animate.
 * Pass start=0, end=0 to animate all characters. */
void wb_node_effect_text_animator_set_range(wb_node *n, int start_char,
                                              int end_char);
/* Set animated properties: offset/scale/rotation/opacity each char
 * animates FROM at u=0 (resolving to identity at u=1). */
void wb_node_effect_text_animator_set_properties(wb_node *n,
                                                   float offset_x,
                                                   float offset_y,
                                                   float scale_val,
                                                   float rotation,
                                                   float opacity);
/* Set easing: 0=linear, 1=ease-in, 2=ease-out, 3=ease-in-out,
 * 4=elastic, 5=bounce. */
void wb_node_effect_text_animator_set_easing(wb_node *n, int ease_type);
/* Set stagger delay per character in seconds. */
void wb_node_effect_text_animator_set_delay(wb_node *n, double delay_per_char);
/* R074 hop 113: position a text node in pixels. */
void wb_node_source_text_pos(wb_node *text_node, int x, int y);
/* #92: resolution-relative title scale. */
int  wb_node_source_text_scale_for(int frame_w);

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

/* R084: render the node graph to H.264 MP4 via libav (no ffmpeg CLI).
 * Pulls frames from `root` at fps intervals up to `duration`, encodes via
 * libx264 (crf=23, veryfast, yuv420p). Honors *cancel between frames;
 * calls prog(ctx, 0..1) for progress. Returns 0 ok, -1 error, -2 cancelled. */
int wb_compositor_render_to_mp4(wb_node *root, const char *out_path,
                                double fps, int w, int h, double duration,
                                volatile int *cancel,
                                wb_export_prog_fn prog, void *prog_ctx);

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

/* G-SF080 v2: param introspection for save/load + editors. */
int         wb_node_graph_param_count(const wb_node_graph *g, int i);
const char *wb_node_graph_param_name(const wb_node_graph *g, int i, int p);
float       wb_node_graph_param_value(const wb_node_graph *g, int i,
                                      int p, double t);
/* G-SF080: graph state mutators (save/load + editor). */
void wb_node_graph_set_label(wb_node_graph *g, int i, const char *label);
void wb_node_graph_set_pos(wb_node_graph *g, int i, float x, float y);
int  wb_node_graph_connect(wb_node_graph *g, int from, int to, int k);

/* R073 hop 82: drive a power window's center along a keyframed path. */
int wb_node_window_track_path(wb_node *win_effect,
                              const double *ts, const float xs[],
                              const float ys[], int n, double dur);

/* ---- Metal GPU acceleration (G12) ------------------------------------ */
/* Initialize Metal compute pipeline. Returns 0 on success, -1 if Metal
 * is unavailable (caller should use CPU path). Call once at startup. */
int  wb_compositor_metal_init(void);
/* Check if Metal GPU processing is available */
int  wb_compositor_metal_is_available(void);
/* Shutdown Metal and release all GPU resources */
void wb_compositor_metal_shutdown(void);

/* GPU-accelerated primary color grade (mirrors CPU op 8).
 * Writes results back to f->px. Returns 0 on success, -1 on failure
 * (caller should fall back to CPU). */
int wb_compositor_metal_process_grade(wb_frame *f,
                                       float lift, float gamma,
                                       float gain, float saturation);

/* GPU-accelerated brightness gain (mirrors CPU op 1). */
int wb_compositor_metal_process_gain(wb_frame *f, float gain);

/* GPU-accelerated deep fry effect. */
int wb_compositor_metal_process_deep_fry(wb_frame *f,
                                          float saturation,
                                          float contrast,
                                          float brightness,
                                          float noise);

/* GPU-accelerated white balance (mirrors CPU op 9). */
int wb_compositor_metal_process_white_balance(wb_frame *f,
                                               float temp,
                                               float tint);

#ifdef __cplusplus
}
#endif

/* ---- After Effects parity: advanced compositing ---- */
wb_node *wb_node_trackmatte_create(void);
void wb_node_trackmatte_set_mode(wb_node *node, int mode);

wb_node *wb_node_frameblend_create(void);
void wb_node_frameblend_set_factor(wb_node *node, float factor);

wb_node *wb_node_adjustment_create(void);
int wb_node_adjustment_add_effect(wb_node *node, wb_node *effect);

/* ---- Shape layers (After Effects parity) ---- */
wb_node *wb_node_source_shape_rect(int w, int h);
wb_node *wb_node_source_shape_ellipse(int w, int h);
wb_node *wb_node_source_shape_polygon(int w, int h, int sides);
wb_node *wb_node_source_shape_star(int w, int h, int points, float inner_radius, float outer_radius);
void wb_node_shape_set_fill(wb_node *node, float r, float g, float b, float a);
void wb_node_shape_set_stroke(wb_node *node, float r, float g, float b, float a, float width);

/* ---- Screen recording (R089: Camtasia parity) ---- */
wb_node *wb_node_source_screen_record(int display_index, int fps);
int wb_screen_display_count(void);
int wb_screen_display_bounds(int index, int *w, int *h);

/* ---- Null objects + parenting (R089: AE parity) ---- */
wb_node *wb_node_create_null(const char *name);
void wb_node_set_parent(wb_node *child, wb_node *parent);
wb_node *wb_node_get_parent(wb_node *child);
int wb_node_get_child_count(wb_node *parent);
wb_node *wb_node_get_child(wb_node *parent, int index);
void wb_node_set_position(wb_node *node, float x, float y);
void wb_node_set_scale(wb_node *node, float sx, float sy);
void wb_node_set_rotation(wb_node *node, float degrees);
void wb_node_set_opacity(wb_node *node, float opacity);
void wb_node_get_position(wb_node *node, float *x, float *y);
void wb_node_get_scale(wb_node *node, float *sx, float *sy);
float wb_node_get_rotation(wb_node *node);
float wb_node_get_opacity(wb_node *node);
void wb_node_get_world_transform(wb_node *node, float *out_x, float *out_y,
                                   float *out_sx, float *out_sy, float *out_rot);

/* ---- Per-layer mask node (R090) ---- */
wb_node *wb_node_effect_mask(int w, int h);
void wb_node_effect_mask_set_feather(wb_node *n, float feather_px);
void wb_node_effect_mask_set_expand(wb_node *n, float expand_px);
void wb_node_effect_mask_set_invert(wb_node *n, int invert);
void wb_node_effect_mask_set_path(wb_node *n, const char *path_str);

/* ---- 3D camera + lights (R089: AE Advanced 3D parity) ---- */
struct wb_3d_camera;
struct wb_light_registry;

struct wb_3d_camera *wb_3d_camera_create(void);
void wb_3d_camera_set_position(struct wb_3d_camera *cam, float x, float y, float z);
void wb_3d_camera_set_target(struct wb_3d_camera *cam, float x, float y, float z);
void wb_3d_camera_set_fov(struct wb_3d_camera *cam, float fov_deg);
void wb_3d_camera_set_dof(struct wb_3d_camera *cam, float focus_distance, float aperture);
void wb_3d_camera_set_near_far(struct wb_3d_camera *cam, float near, float far);
void wb_3d_camera_update_matrices(struct wb_3d_camera *cam, float aspect);
void wb_3d_camera_get_view_matrix(struct wb_3d_camera *cam, float *out_16);
void wb_3d_camera_get_proj_matrix(struct wb_3d_camera *cam, float *out_16);
void wb_3d_camera_destroy(struct wb_3d_camera *cam);

struct wb_light_registry *wb_light_registry_create(void);
int wb_light_add_point(struct wb_light_registry *r, float x, float y, float z,
                        float r_col, float g, float b, float intensity, float range);
int wb_light_add_spot(struct wb_light_registry *r, float px, float py, float pz,
                       float dx, float dy, float dz,
                       float r_col, float g, float b, float intensity,
                       float angle_deg, float softness);
int wb_light_add_directional(struct wb_light_registry *r, float dx, float dy, float dz,
                              float r_col, float g, float b, float intensity);
int wb_light_add_ambient(struct wb_light_registry *r, float r_col, float g, float b);
void wb_light_set_shadows(struct wb_light_registry *r, int light_idx, int enable);
void wb_light_remove(struct wb_light_registry *r, int light_idx);
void wb_light_clear(struct wb_light_registry *r);
int wb_light_count(struct wb_light_registry *r);
void wb_light_registry_destroy(struct wb_light_registry *r);

struct wb_node *wb_node_source_3d_camera(int w, int h);

/* ---- AI auto-caption / speech-to-text subtitle burn-in (wb_stt_caption.c) ---- */
/* Set the language for STT transcription (ISO-639-1: "en", "es", "auto"). */
int  wb_stt_set_language(const char *lang);
/* Transcribe audio from a video clip in the edit graph via whisper.cpp.
 * Returns 0 on success, -1 on error. */
int  wb_stt_process_audio(wb_edit_graph *g, int track, int clip_idx);
/* Get the last transcription result as SRT text. Returns bytes written or -1. */
int  wb_stt_get_transcription_result(char *buf, int bufsize);
/* Burn SRT subtitles into a video clip. Parses SRT, caches it, sets overlay.
 * Returns 0 on success, -1 on error. */
int  wb_stt_burn_subtitles(wb_edit_graph *g, int track, int clip_idx, const char *srt);
/* Burn subtitles directly onto a frame at time time_ms (uses wb_ui_text_to_rgba). */
void wb_stt_burn_on_frame(wb_frame *f, int time_ms);
/* Get the active caption text at time_ms ("" if none). */
const char *wb_stt_get_caption_at(int time_ms);
/* Configure subtitle style: scale, position (normalized 0..1), RGBA color. */
void wb_stt_set_style(float scale, float x, float y, uint32_t color);
/* Enable/disable subtitle overlay. */
void wb_stt_set_enabled(int enabled);
/* Get parsed SRT entry count. */
int  wb_stt_get_entry_count(void);
/* Get the SRT file path for ffmpeg export burn. */
const char *wb_stt_get_srt_path(void);

/* ---- Real-time preview (R090: GPU playback) ---- */
struct wb_preview;
typedef enum { WB_PREVIEW_STOPPED, WB_PREVIEW_PLAYING, WB_PREVIEW_PAUSED, WB_PREVIEW_SCRUBBING } wb_preview_state;

struct wb_preview *wb_preview_create(wb_node *root_node, int w, int h, double fps);
void wb_preview_set_duration(struct wb_preview *p, double duration_sec);
void wb_preview_set_loop(struct wb_preview *p, int loop);
void wb_preview_set_speed(struct wb_preview *p, double speed);
void wb_preview_play(struct wb_preview *p);
void wb_preview_pause(struct wb_preview *p);
void wb_preview_stop(struct wb_preview *p);
void wb_preview_seek(struct wb_preview *p, double time_sec);
double wb_preview_get_time(struct wb_preview *p);
wb_preview_state wb_preview_get_state(struct wb_preview *p);
double wb_preview_get_fps(struct wb_preview *p);
int wb_preview_get_frame(struct wb_preview *p, uint8_t *out_rgba);
void wb_preview_destroy(struct wb_preview *p);

/* ---- DVD/Blu-ray authoring (R090) ---- */
struct wb_dvd_project;
typedef enum { WB_DVD_FORMAT_DVD5, WB_DVD_FORMAT_DVD9, WB_DVD_FORMAT_BD25, WB_DVD_FORMAT_BD50 } wb_dvd_format;
typedef struct { float x, y, w, h; int target_title; } wb_dvd_button;

struct wb_dvd_project *wb_dvd_author_create(void);
int wb_dvd_author_add_title(struct wb_dvd_project *p, const char *video_path, const char *audio_path, double duration_sec);
int wb_dvd_author_set_menu(struct wb_dvd_project *p, const char *bg_image_path, const wb_dvd_button *buttons, int button_count);
int wb_dvd_author_set_chapters(struct wb_dvd_project *p, int title_idx, const double *times, int count);
int wb_dvd_author_export(struct wb_dvd_project *p, const char *output_dir, int format);
const char *wb_dvd_author_get_error(struct wb_dvd_project *p);
void wb_dvd_author_destroy(struct wb_dvd_project *p);

/* ---- DVD-Video Virtual Machine (VM) instruction encoder (R090) ---- */
/* DVD VM register file: 16 GPRMs + 24 SPRMs, all 16-bit unsigned. */
typedef struct wb_dvd_vm {
    uint16_t gprm[16];  /* General Parameter Registers 0-15 */
    uint16_t sprm[24];  /* System Parameter Registers 0-23 */
} wb_dvd_vm;

/* Arithmetic operations for Set GPRM instructions */
typedef enum {
    DVD_VM_ARITH_NOP    = 0,  /* no operation */
    DVD_VM_ARITH_ASSIGN = 1,  /* dst = src */
    DVD_VM_ARITH_SWAP   = 2,  /* swap dst and src */
    DVD_VM_ARITH_ADD    = 3,  /* dst += src */
    DVD_VM_ARITH_SUB    = 4,  /* dst -= src */
    DVD_VM_ARITH_MUL    = 5,  /* dst *= src */
    DVD_VM_ARITH_DIV    = 6,  /* dst /= src */
    DVD_VM_ARITH_MOD    = 7,  /* dst %= src */
    DVD_VM_ARITH_RANDOM = 8,  /* dst = random(1..src) */
    DVD_VM_ARITH_AND    = 9,  /* dst &= src */
    DVD_VM_ARITH_OR     = 10, /* dst |= src */
    DVD_VM_ARITH_XOR    = 11  /* dst ^= src */
} wb_dvd_vm_arith_op;

/* Compare operations */
typedef enum {
    DVD_VM_CMP_NEVER    = 0,  /* always false */
    DVD_VM_CMP_EQ       = 1,  /* reg == src */
    DVD_VM_CMP_NEQ      = 2,  /* reg != src */
    DVD_VM_CMP_GT       = 3,  /* reg > src */
    DVD_VM_CMP_GTE      = 4,  /* reg >= src */
    DVD_VM_CMP_LT       = 5,  /* reg < src */
    DVD_VM_CMP_LTE      = 6,  /* reg <= src */
    DVD_VM_CMP_ALWAYS   = 7   /* always true */
} wb_dvd_vm_cmp_op;

/* Source type for Set/Cmp operands: immediate or register */
typedef enum {
    DVD_VM_SRC_IMM  = 0,  /* immediate 16-bit value */
    DVD_VM_SRC_GPRM = 1,  /* GPRM register */
    DVD_VM_SRC_SPRM = 2   /* SPRM register */
} wb_dvd_vm_src_type;

/* Link types for Link and Jump/Call instructions */
typedef enum {
    DVD_VM_LINK_NONE        = 0,
    DVD_VM_LINK_PGCN        = 1,  /* LinkPGCN / LinkPGCN */
    DVD_VM_LINK_PTTN        = 2,  /* LinkPTTN */
    DVD_VM_LINK_PGN         = 3,  /* LinkPGN */
    DVD_VM_LINK_CN          = 4,  /* LinkCN */
    DVD_VM_LINK_RSM         = 5,  /* RSM */
    DVD_VM_JUMP_TT          = 6,  /* JumpTT */
    DVD_VM_JUMP_VTS_TT      = 7,  /* JumpVTS_TT */
    DVD_VM_JUMP_VTS_PTT     = 8,  /* JumpVTS_PTT */
    DVD_VM_JUMP_SS_FP       = 9,  /* JumpSS_FP */
    DVD_VM_JUMP_SS_MENU     = 10, /* JumpSS_MENU */
    DVD_VM_JUMP_SS_VMGM     = 11, /* JumpSS_VMGM */
    DVD_VM_CALL_SS_FP       = 12, /* CallSS_FP */
    DVD_VM_LINK_SFP_PGCN    = 13  /* Link_SFP_PGCN */
} wb_dvd_vm_link_type;

/* Register access */
void     wb_dvd_vm_init(wb_dvd_vm *vm);
void     wb_dvd_vm_set_gprm(wb_dvd_vm *vm, int reg, uint16_t val);
void     wb_dvd_vm_set_sprm(wb_dvd_vm *vm, int reg, uint16_t val);
uint16_t wb_dvd_vm_get_gprm(wb_dvd_vm *vm, int reg);
uint16_t wb_dvd_vm_get_sprm(wb_dvd_vm *vm, int reg);

/* Instruction emitters — each writes exactly 8 bytes to out */
void wb_dvd_vm_emit_nop(uint8_t *out);
void wb_dvd_vm_emit_goto(uint8_t *out, int cmd_offset);
void wb_dvd_vm_emit_break(uint8_t *out);
void wb_dvd_vm_emit_set_gprm(uint8_t *out, int dst_reg, int op, int src_type, uint16_t src_val);
void wb_dvd_vm_emit_set_sprm(uint8_t *out, int dst_reg, uint16_t val);
void wb_dvd_vm_emit_compare(uint8_t *out, int reg, int cmp_op, int src_type, uint16_t src_val);
void wb_dvd_vm_emit_link_pgcn(uint8_t *out, int pgcn);
void wb_dvd_vm_emit_link_pttn(uint8_t *out, int pttn);
void wb_dvd_vm_emit_jump_tt(uint8_t *out, int title);
void wb_dvd_vm_emit_jump_vts_tt(uint8_t *out, int vts, int title);
void wb_dvd_vm_emit_call_ss(uint8_t *out, int pgcn);
void wb_dvd_vm_emit_set_link(uint8_t *out, int dst_reg, int op, int src_type, uint16_t src_val, int link_type, int link_arg);
void wb_dvd_vm_emit_conditional(uint8_t *out, int cmp_reg, int cmp_op, int cmp_src_type, uint16_t cmp_val, int true_action_count, uint8_t *true_actions);

/* ---- HDR preview (R090) ---- */
struct wb_hdr_preview;
typedef enum { WB_HDR_DISPLAY_SDR = 0, WB_HDR_DISPLAY_HDR10, WB_HDR_DISPLAY_HLG, WB_HDR_DISPLAY_DOLBY_VISION } wb_hdr_display_mode;
typedef enum { WB_HDR_CS_REC709 = 0, WB_HDR_CS_REC2020, WB_HDR_CS_DCIP3 } wb_hdr_color_space;
struct wb_hdr_preview *wb_hdr_preview_create(int w, int h);
void wb_hdr_set_display_mode(struct wb_hdr_preview *hdr, int mode);
void wb_hdr_set_peak_brightness(struct wb_hdr_preview *hdr, float nits);
void wb_hdr_set_color_space(struct wb_hdr_preview *hdr, int cs);
void wb_hdr_set_tone_map(struct wb_hdr_preview *hdr, int method);
void wb_hdr_set_exposure(struct wb_hdr_preview *hdr, float exposure);
void wb_hdr_process_frame(struct wb_hdr_preview *hdr, wb_frame *frame_in, wb_frame *frame_out);
void wb_hdr_apply_metadata(struct wb_hdr_preview *hdr, wb_frame *frame, float max_cll, float max_fall);
void wb_hdr_preview_destroy(struct wb_hdr_preview *hdr);

/* ---- YouTube upload (R090) ---- */
int wb_youtube_set_credentials(const char *client_id, const char *client_secret, const char *refresh_token);
int wb_youtube_upload(const char *video_path, const char *title, const char *description, const char *tags, int privacy);
int wb_youtube_get_upload_status(double *progress, char *status_buf, int bufsize);
int wb_youtube_cancel_upload(void);
int wb_youtube_set_thumbnail(const char *video_path, const char *thumbnail_path);

/* ---- SVG import (R090) ---- */
int wb_svg_import(const char *svg_path, int target_w, int target_h, wb_node **out_nodes, int max_nodes);
int wb_svg_parse_path(const char *path_d, float *verts_x, float *verts_y, int *vert_count, int max_verts);
int wb_svg_get_fill_color(wb_node *node, float *r, float *g, float *b, float *a);
int wb_svg_get_stroke(wb_node *node, float *r, float *g, float *b, float *a, float *width);
int wb_svg_get_transform(wb_node *node, float *transform_out);

/* ---- DVD VM constants (R090) ---- */
#define VM_OP_NOP               0
#define VM_OP_ASSIGN            1
#define VM_OP_SWAP              2
#define VM_OP_ADD               3
#define VM_OP_SUB               4
#define VM_OP_MUL               5
#define VM_OP_DIV               6
#define VM_OP_MOD               7
#define VM_OP_RND               8
#define VM_OP_AND               9
#define VM_OP_OR                10
#define VM_OP_XOR               11
#define VM_CMP_NEVER            0
#define VM_CMP_EQ               1
#define VM_CMP_NEQ              2
#define VM_CMP_GT               3
#define VM_CMP_GTE              4
#define VM_CMP_LT               5
#define VM_CMP_LTE              6
#define VM_CMP_ALWAYS           7
#define VM_SRC_IMM              0
#define VM_SRC_GPRM             1
#define VM_SRC_SPRM             2
#define SPRM_MENU_LANGUAGE      0
#define SPRM_AUDIO_STREAM       1
#define SPRM_SUBPIC_STREAM      2
#define SPRM_ANGLE              3
#define SPRM_TITLE_NUMBER       4
#define SPRM_VTS_TITLE_NUMBER   5
#define SPRM_PGC_NUMBER         6
#define SPRM_CHAPTER_NUMBER     7
#define SPRM_HIGHLIGHT_BUTTON   8
#define SPRM_NAV_TIMER          9
#define SPRM_NAV_TIMER_PGCN     10
#define SPRM_KARAOKE_MIX        11
#define SPRM_PARENTAL_COUNTRY   12
#define SPRM_PARENTAL_LEVEL     13
#define SPRM_VIDEO_PREFERENCE   14
#define SPRM_AUDIO_CAPS         15
#define VM_LINK_LINKPGCN        0x01
#define VM_LINK_LINKPTTN        0x04
#define VM_LINK_LINKPGN         0x05
#define VM_LINK_LINKCN          0x06
#define VM_LINK_RSM             0x08

/* DVD VM and game forward declarations */
typedef struct wb_dvd_vm wb_dvd_vm;
typedef struct wb_dvd_game wb_dvd_game;

/* DVD game builder API */
wb_dvd_game *wb_dvd_game_create(struct wb_dvd_project *proj, const char *name);
void wb_dvd_game_add_score(wb_dvd_game *game, int points_per_correct, int points_per_wrong);
void wb_dvd_game_add_question(wb_dvd_game *game, const char *video_path, int correct_button, int num_buttons);
void wb_dvd_game_set_branching(wb_dvd_game *game, int score_threshold, int target_pgcn);
void wb_dvd_game_add_easter_egg(wb_dvd_game *game, int button_combo[], int combo_length, int target_pgcn);
void wb_dvd_game_set_timer(wb_dvd_game *game, int seconds, int timeout_pgcn);
void wb_dvd_game_add_hidden_button(wb_dvd_game *game, int x, int y, int w, int h, int target_pgcn);
void wb_dvd_game_set_parental(wb_dvd_game *game, int level, int password);
int wb_dvd_game_generate_vm(wb_dvd_game *game, uint8_t *out, int max_len);
int wb_dvd_game_build(wb_dvd_game *game);
void wb_dvd_game_destroy(wb_dvd_game *game);

#endif /* WUBUS_WBUS_COMPOSITOR_H */
