# Big Mac Video Editor — Architecture Gap Analysis (R085)

## Current State: ~40% → ~70%

The video editing pipeline now works end-to-end through the compositor node graph.

## What's WIRED (The 70%)

### Core Pipeline
- `src/wb_video_node.c` — `wb_node_source_video()`: FFmpeg decoder → compositor source node
- `src/wb_vfx_node.c` — 7 VFX nodes: deep_fry, vhs, rgb_glitch, posterize, vignette, chromatic, camera_shake
- `src/wb_lut_node.c` — 3D LUT color grading (.cube files)
- `src/wb_video_fx_pro.c` — stabilize, chromakey, transform_pro
- `src/wb_audio_reactive_node.c` — audio features → video FX
- `src/wb_compositor_encode.c` — node graph → H.264 via libav (no ffmpeg CLI)
- `src/wb_edit.c` — Multi-track EDL: clips, transitions, FX chains, nested sequences, color management, subtitle burn-in
- `src/wb_edit.c` — `wb_edit_render_to_mp4()`: per-frame eval → encode loop

### Edit Model
- Multi-track timeline with clips
- Transitions between adjacent clips
- Per-clip FX chains (arbitrary node stacking)
- Nested sequences (sub-graphs as clips)
- Color management pipeline (CST → tonemap → output)
- Keyframed FX params via compositor param bus
- Scene detection auto-cutting
- Subtitle/caption burn-in during export

### Agent API
- `edit-new`, `edit-add-track`, `edit-add-clip`, `edit-split`, `edit-move`
- `edit-trans`, `edit-fx` (12 effect types), `edit-eval`, `edit-render`
- `edit-new-seq`, `edit-add-seq-clip`, `edit-auto-cut`
- `edit-color-on/off`, `edit-input-cs`, `output-cs`, `tonemap`
- `edit-subtitle`, `edit-subtitle-pos/size/color`
- `edit-state`

### DAW UI
- FUSION tab: node graph drawing + live preview texture infrastructure
- Workspace tier ribbon (AUDIO/VIDEO/FUSION/3D-CGI/AGI/PERF)
- Keyframe editor for node params (G24)

### Tests
- test_edit.c: 57 checks (edit graph, nested sequences, color management)
- test_procedural.c: 14 checks (text/scene/color sources, PPM export)
- test_video_fx_pro.c: 11 checks (stabilize, chromakey, transform, audio-reactive)
- All passing. Selftest: 753/0.

## What's Still Missing (The 30%)

### Depth & Polish
1. **Proxy workflow** — edit with low-res proxies, export with full-res sources
2. **Render queue** — multiple exports queued and processed sequentially
3. **Compositor Pro integration** — Fusion/Nuke node types (input/blend/key/transform/color/output) wired to video
4. **Keyframe editor for video FX** — DAW UI to keyframe effect params over time
5. **Audio track mixing for video** — multi-track audio synchronized with video timeline
6. **Waveform/thumbnail display** — audio waveforms and video thumbnails in the timeline
7. **Undo/redo for edit graph** — structural undo for edit operations
8. **Project save/load** — serialize edit graph to disk
9. **Batch/agent scripting** — full EDL scripting via agent

### Advanced Features
10. **Motion tracking overlay** — attach graphics to tracked points
11. **Speed ramping** — variable playback speed within a clip
12. **Picture-in-picture** — multiple video layers with blending
13. **Audio-reactive auto-cut** — cut on beats/transients
14. **AI-powered features** — auto-caption, scene summarization
15. **OFX plugin hosting** — third-party video effect plugins
16. **GPU acceleration** — Metal/CUDA for real-time playback

### SOTA Differentiators
17. **Color science** — ACES, wide gamut, HDR grading
18. **Collaborative editing** — multiple users on same project
19. **Remote rendering** — render farm support
20. **Format support** — ProRes, DNxHD, RAW, EXR
21. **Scopes** — waveform, vectorscope, histogram
22. **Audio mixing** — full DAW-style mixing with VST/CLAP plugins
