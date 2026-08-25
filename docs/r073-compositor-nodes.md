# R073 Compositor Node Catalog

Status of the pull-based node compositor after hops 39–77 (Aug 2026).
All nodes: pure C11, RoI-scoped pulls, params keyframable via the G11
param bus (`wb_node_add_param`). Gate: `build/wb_test_compositor`.

## Effect nodes (`wb_node_effect(op, gain)`)

| op | Name | Params | Notes |
|----|------|--------|-------|
| 0 | Bypass | — | passthrough |
| 1 | Gain | gain | RGB multiply |
| 2 | Invert alpha | — | matte flip |
| 3 | Chroma key | key_tol, key_color, spill | green/blue/red screens; soft edge band; spill suppression clamps key channel toward max(others) |
| 4 | Gaussian blur | blur | separable two-pass 3-tap box |
| 5 | Luma key | lum_thr | Rec.709 luma threshold, screen-blend style |
| 6 | Vignette | vig | radial falloff past 50% radius |
| 7 | Glow | glow_thr | soft-knee bloom above luma threshold |
| 8 | Primary grade | lift/gamma/gain/sat | gamma on positives, sat around Rec.709 luma |
| 9 | White balance | temp/tint | RGB channel gains (positive temp = warm) |
| 10 | Tone curves | cur_blk/shd/hig/wht | piecewise-smoothstep, identity when unbound |
| 11 | HSL secondary | hue_c/hue_w/sec_sat + win_* | hue qualifier with soft selection + power window; win_shape 0=circle 1=rect (Chebyshev) 2=ellipse (win_rx/win_ry aniso); win_rot rotates all shapes |

## Transitions (`wb_node_transition(op, dur)` + `wb_transition_add` A,B)

| op | Name |
|----|------|
| 0 | Crossfade |
| 1 | Dip-to-black |
| 2 | Linear wipe — `wb_transition_dir`: 0 L→R, 1 R→L, 2 T→B, 3 B→T; feathered edge (8% smoothstep band) |
| 3 | Iris (circular reveal) |
| 4 | Slide (B over stationary A) |
| 5 | Push (both translate) |
| 6 | Noise dissolve (deterministic per-pixel hash jitter ±15%) |
| 7 | Map dissolve (third input's luma = per-pixel threshold) |

## Sources

- `wb_node_source_color(r,g,b,a,w,h)`
- `wb_node_source_anim(anim,w,h)` — CGI scenes as compositor inputs
- `wb_node_source_text(text,scale,r,g,b,a,w,h)` + `_anim(mode,dur)` — presets: 1 typewriter, 2 slide-in, 3 fade-out, 4 fade-in/out cycle; multi-line via `\n`; drop shadow built in
- Audio-reactive: `wb_cgi_audio_pulse`, `wb_cgi_beat_pulse` (BPM grid),
  `wb_cgi_band_pulse` (FFT band), `wb_cgi_visualizer_build` (one-call
  bass/mid/high scene), `wb_cgi_camera_shake` (G27 transients).
  Agent command: `cgi-viz <track> <clip> [base] [amount]`.

## Ownership pitfalls (learned the hard way)

- Composite and Cache nodes OWN their children; source/effect nodes do
  NOT. Callers must not destroy composite children twice.
- `wb_anim_add_object` stores the mesh pointer without copying — caller
  keeps it alive until after `wb_anim_free`.
- Transition input slots are pre-allocated (2); `wb_transition_add`
  fills them, max 2 (+ map as third for op 7).
- Composite output frames are 4096×4096 RoD — probe with the frame's
  stride, not the RoI size.
