# Big Mac Video Editor — Architecture Gap Analysis (R084)

## The Problem

We have ~216 src modules and 57 tests, but the video editing pipeline is
fundamentally broken: the compositor node graph (the "brain" of a video
editor) is never wired to the video decoder/encoder (the "eyes"). The export
path is a 150-line `snprintf` that builds an ffmpeg command string and `fork`s
a shell. This is the same anti-pattern as the tools we just deleted — just
bigger.

## What Exists (The 2%)

### Engine side (real C11, no shell):
- `src/wb_ytp.c` — audio FX: stutter, pitch, earrape, reverse, time_stretch,
  sentence_mix, word_salad, datamosh, vine_boom
- `src/wb_vfx.c` — video FX: blend modes, LUT3D, color_correct, transitions,
  camera_shake, chromatic_aberration, deep_fry, VHS, RGB_glitch, posterize,
  vignette
- `src/wb_video.c` — FFmpeg libav decode/encode, proxy generation, SDL2 preview
- `src/wb_compositor.c` — pull-based RoI/RoD node compositor (Natron-style)
- `src/wb_compositor_pro.c` — Fusion/Nuke-style node graph (input/blend/key/
  transform/color/output)
- `src/wb_transitions_pro.c` — 28 transition types with params
- `src/wb_color_grading.c` — color grading

### UI side:
- `tools/wb_daw.c` — 6000-line SDL2 app with 12 tabs, workspace tier ribbon
  (AUDIO/VIDEO/FUSION/3D-CGI/AGI/PERF), node graph editor, keyframe editor

### Infrastructure:
- `wbus_agent.h` — headless command API (import/split/polish/edl/export)
- `wbus_export_job.h` — background render queue with cancel
- `wbus_workspace.h` — DaVinci-Resolve-style tier switcher

## What's Missing (The 98%)

### 1. Video → Compositor Bridge (CRITICAL)
The video decoder (`wb_video_decoder`) produces RGBA frames but they go
directly to SDL2 texture blit. There is NO `wb_node_source_video()` that
feeds decoded frames into the compositor node graph. Without this, NO effect
from `wb_vfx.c` or `wb_compositor.c` can ever be applied to video.

**Need:** A source node that wraps `wb_video_decoder` and implements the
`wb_node_pull_fn` contract: pull(time, roi) → decode frame → return wb_frame.

### 2. Compositor → Export Bridge (CRITICAL)
The export path (`wb_video_export_full`) builds an ffmpeg CLI string. It
bypasses the compositor entirely. The node graph can render frames but
there's no `wb_compositor_export_to_encoder()` that pulls frames at each
time and feeds them to the encoder.

**Need:** A render loop that walks the node graph at fps intervals, pulls
wb_frames, and encodes them via libav (not ffmpeg CLI).

### 3. Edit Decision List (EDL) Model (CRITICAL)
The session has video clips with `start_in_source`, `duration`, `timeline_pos`
but there's no edit graph. A real NLE needs:
- Multi-track timeline with clips
- Transitions between adjacent clips (the compositor's `wb_node_transition`)
- Effects stack per clip (the compositor's node graph)
- Nested sequences (sub-graphs)

**Need:** An edit model that maps timeline positions → node graph evaluation.

### 4. Keyframed Parameters on Video FX (MISSING)
The compositor has `wb_param_track` and `wb_node_add_param()` for keyframable
params, but no video FX node uses them. The `wb_vfx.c` functions take fixed
values (e.g., `wb_effect_deep_fry(rgba, w, h, intensity)` — intensity is a
constant, not a keyframed track).

**Need:** Video FX nodes that read param tracks per-frame from the compositor
param bus.

### 5. Agent API for Video Editing (MISSING)
The agent API (`wbus_agent.h`) has `import`, `split`, `export` but no:
- `effect <clip> <fx> [params]` — apply an effect
- `transition <clipA> <clipB> <type> <duration>` — add transition
- `node_add <type>`, `node_connect <from> <to>` — build node graph
- `keyframe <param> <t> <value>` — set keyframe

**Need:** Extend the agent protocol to drive the compositor graph.

### 6. Real-time Preview Through Node Graph (MISSING)
The DAW's FUSION tab draws the node graph but never pulls frames through it.
The video preview in the MEDIA tab uses `wb_video_decoder` → SDL2 texture
directly, bypassing all effects.

**Need:** Preview pulls frames through the node graph at proxy resolution.

### 7. Procedural Video Generation (MISSING)
The compositor has source nodes (color, text, scene, anim) but no way to
render them to a video file without the export bridge. This means no
title cards, no lower-thirds, no procedural backgrounds.

### 8. Audio-Reactive Video (MISSING)
The engine has FFT, beat detection, audio-reactive parameters, but the
video side never reads them. No audio-driven effects, no beat-synced cuts.

## The Methodology

A video editor is a **pull-based node graph**:

```
[Video Source Node] → [Effect Node] → [Transform Node] → [Composite Node] → [Output Node]
                                                                          ↓
                                                                    [Encoder → file]
```

Each node:
1. Declares its output format (resolution, framerate, color space)
2. On pull(time, roi), requests inputs from upstream nodes
3. Computes its output from inputs
4. Caches results (bounded LRU by content hash)

The timeline is a **time→graph mapping**:
- At each frame time T, evaluate the graph rooted at the output node
- The output node pulls its input at T, which pulls its input at T, etc.
- Source nodes map T to their source time (accounting for speed ramps,
  loops, timeline position)

## What Needs To Be Built (Priority Order)

### Phase 1: Wire the Existing Pieces
1. `wb_node_source_video()` — video decoder as a compositor source node
2. `wb_node_effect_*()` — wrap existing `wb_vfx.c` functions as effect nodes
3. `wb_compositor_export_render_loop()` — pull frames → encode via libav
4. Wire export_job to use the render loop instead of ffmpeg CLI

### Phase 2: Edit Model
5. Multi-track edit graph (tracks → clips → transitions)
6. Timeline→graph mapping (time T → which clips are active → build subgraph)
7. Keyframed FX params on video effect nodes

### Phase 3: Agent + UI
8. Agent API for video editing (effect, transition, node_add, keyframe)
9. Real-time preview through node graph in FUSION tab
10. Keyframe editor for video FX params (reuse G24 editor)

### Phase 4: Advanced
11. Audio-reactive video (FFT → param tracks → FX nodes)
12. Procedural generation (text/title/background → export)
13. Nested sequences (sub-graphs as clips)
14. Color management pipeline (CST nodes → tonemap → output)

## Why Previous Attempts Failed

The deleted tools (wb_ytp_studio, wb_ytp_compose, etc.) tried to build a
video editor by shelling out to ffmpeg. This is fundamentally wrong because:
- No node graph → no compositing, no effects stacking
- No param tracks → no keyframing, no animation
- No edit model → no multi-track, no transitions
- No bridge to engine → can't use wb_vfx.c, wb_ytp.c, wb_compositor.c

The engine has ALL the pieces. They just need to be wired together through
the node graph.
