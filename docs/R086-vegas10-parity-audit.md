# Big Mac vs Sony Vegas Pro 10 — Feature Parity Audit

## Vegas 10 Feature List (from Sound on Sound review + Wikipedia)

### VIDEO FEATURES

| # | Feature | Vegas 10 | Big Mac | Status |
|---|---------|----------|---------|--------|
| 1 | Multi-track timeline | ✅ | ✅ wb_edit_graph with tracks | PASS |
| 2 | Non-linear editing | ✅ | ✅ clips with start/duration/timeline_pos | PASS |
| 3 | Video transitions (crossfade, wipe, etc.) | ✅ | ✅ wb_node_transition + 28 pro transitions | PASS |
| 4 | Video effects (color correct, blur, etc.) | ✅ | ✅ 12+ VFX nodes | PASS |
| 5 | Keyframing for effects | ✅ (limited in V10) | ✅ wb_edit_set_keyframe + param bus | PASS |
| 6 | 3D stereoscopic editing | ✅ | ❌ | **FAIL** |
| 7 | Image stabilization | ✅ | ✅ wb_node_effect_stabilize | PASS |
| 8 | Motion tracking | ❌ (V10 lacks this) | ✅ wb_node_effect_motion_track | N/A |
| 9 | Multi-camera editing | ✅ | ❌ | **FAIL** |
| 10 | Closed captioning / subtitles | ✅ | ✅ wb_edit_set_subtitle + bitmap font | PASS |
| 11 | GPU acceleration (CUDA) | ✅ (encode only) | ❌ | **FAIL** |
| 12 | 4K support | ❌ (V9 added 4K) | ✅ via libav | PASS |
| 13 | Speed ramping / varispeed | ✅ | ✅ cl->speed in evaluation | PASS |
| 14 | Nested sequences | ❌ | ✅ wb_edit_sequence | PASS |
| 15 | Proxy editing | ❌ | ✅ wb_edit_generate_proxy | PASS |

### AUDIO FEATURES

| # | Feature | Vegas 10 | Big Mac | Status |
|---|---------|----------|---------|--------|
| 16 | Multi-track audio | ✅ | ✅ audio_clips per track | PASS |
| 17 | Audio mixing console | ✅ | ❌ (no mixer UI) | **FAIL** |
| 18 | Audio effects (per-event) | ✅ | ❌ (no audio FX chain) | **FAIL** |
| 19 | 5.1 surround mixing | ✅ | ❌ (stereo only) | **FAIL** |
| 20 | Input buses (26) | ✅ | ❌ | **FAIL** |
| 21 | VU meters per track | ✅ | ❌ | **FAIL** |
| 22 | Pan faders per track | ✅ | ❌ | **FAIL** |
| 23 | Broadcast wave format | ✅ | ❌ | **FAIL** |
| 24 | Audio render to WAV | ✅ | ✅ wb_wav_write_pcm16 | PASS |
| 25 | AAC audio in MP4 | ✅ | ✅ wb_audio_mux_to_mp4 | PASS |
| 26 | Volume automation | ✅ | ✅ wb_edit_set_audio_volume | PASS |

### FORMAT SUPPORT

| # | Feature | Vegas 10 | Big Mac | Status |
|---|---------|----------|---------|--------|
| 27 | AVCHD support | ✅ | ✅ via libav | PASS |
| 28 | DSLR H.264 support | ✅ | ✅ via libav | PASS |
| 29 | XDCAM support | ✅ | ✅ via libav | PASS |
| 30 | Red One support | ✅ | ✅ via libav | PASS |
| 31 | QuickTime support | ✅ | ✅ via libav | PASS |
| 32 | ProRes support | ❌ (V10 lacks) | ✅ via libav | PASS |
| 33 | AVI output | ✅ | ✅ via libav | PASS |
| 34 | MP4/H.264 output | ✅ (Sony AVC) | ✅ native H.264 | PASS |
| 35 | Windows Media | ✅ | ✅ via libav | PASS |
| 36 | DVD authoring | ✅ (DVD Architect) | ❌ | **FAIL** |
| 37 | Blu-ray authoring | ❌ (V10 lacks) | ❌ | N/A |

### WORKFLOW

| # | Feature | Vegas 10 | Big Mac | Status |
|---|---------|----------|---------|--------|
| 38 | Project media bin | ✅ | ❌ | **FAIL** |
| 39 | Trimmer / source monitor | ✅ | ❌ | **FAIL** |
| 40 | Keyboard shortcuts | ✅ | ✅ (N/I/S/E + more) | PASS |
| 41 | Undo/redo | ✅ | ❌ (no edit graph undo) | **FAIL** |
| 42 | Project save/load | ✅ | ✅ .bedit format | PASS |
| 43 | Batch export | ✅ | ✅ export queue | PASS |
| 44 | Scripting/automation | ✅ | ✅ agent API (25+ cmds) | PASS |
| 45 | OFX plugin support | ✅ (new in V10) | ✅ OFX host exists | PASS |

## FAILURES TO FIX

1. **3D stereoscopic editing** — needs stereo pair import, 3D depth adjustment
2. **Multi-camera editing** — needs sync + angle switching UI
3. **GPU acceleration** — needs Metal/CUDA for real-time playback
4. **Audio mixer UI** — needs on-screen mixer with VU meters
5. **Audio effects chain** — needs per-clip audio FX
6. **5.1 surround** — needs multi-channel audio support
7. **Input buses** — needs external audio I/O routing
8. **DVD authoring** — needs DVD menu system
9. **Media bin** — needs clip organization UI
10. **Trimmer/source monitor** — needs pre-timeline clip editing
11. **Undo/redo for edit graph** — needs structural undo
