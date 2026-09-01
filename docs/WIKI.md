# Big Mac — Multimedia Suite Pipeline

> **One engine. One session. Every medium.**
> DAW + NLE + Compositor + CGI + AGI + YTP — all tabs in one app.

Big Mac is a **full multimedia production suite** built in pure C11 on a dual-core
iMac. It is NOT a DAW with video bolted on, NOT an NLE with audio tacked on. It is
one engine that happens to do everything: record audio, edit video, composite
scenes, render CGI, orchestrate AI tasks, and produce YTPs/meme videos.

**The sauce:** every feature shares ONE session model (`wb_session`), ONE transport,
ONE render path. Audio and video never get tangled because they're the same graph.

---

## Architecture Map

```
┌─────────────────────────────────────────────────────────────────┐
│                        tools/wb_daw.c                           │
│  SDL2 window • 1360×800 • transport • toolbar • tabs            │
│  Tabs: ARRANGE | PAD | STEP | SESSION | MEDIA | EDIT |          │
│        CAPTIONS | EXPORT | FX | COLOR | MOTION | TEXT           │
│  Ribbon: AUDIO | VIDEO | FUSION | 3D-CGI | AGI | PERF          │
├─────────────────────────────────────────────────────────────────┤
│                     Workspace Tier System                        
│  wb_workspace — flips the app between modes, ONE session alive   │
│  AUDIO→arrange  VIDEO→media  FUSION→node  3D-CGI→scene  AGI→task│
├─────────────────────────────────────────────────────────────────┤
│                        Engine Core                              │
│  wb_engine  │ wb_transport │ wb_session │ wb_cmd (SPSC queue)  │
│  Sample-accurate pull-based render → CoreAudio / offline file    │
├──────────────┼──────────────┼──────────────┬────────────────────┤
│   AUDIO      │   VIDEO      │  COMPOSITOR  │  CGI / AGI         │
│   DSP graph  │   decoder    │  node graph  │  scene model       │
│   instruments│   proxy/480p │  OFX bridge  │  task orchestrator │
│   FX chain   │   export     │  keyframes   │  agent protocol    │
├──────────────┴──────────────┴──────────────┴────────────────────┤
│                    Session Model (wb_session)                    │
│  tracks[] • clips[] • notes[] • markers[] • automation lanes    │
│  Save/Load: .wbus (text) + ShadowBin JSON sidecar               │
└─────────────────────────────────────────────────────────────────┘
```

---

## Workspace Tiers (the ribbon)

Big Mac uses a **DaVinci Resolve-style workspace switcher**. The bottom ribbon
flips between tiers. Each tier changes what the tabs show, but the session stays
alive — you can be mixing audio, flip to video to check a cut, flip back.

| Tier | Label | What it does | Tab focus |
|------|-------|-------------|-----------|
| 0 | **AUDIO** | DAW core — arrange, mix, MIDI, instruments | ARRANGE, PAD, STEP, SESSION |
| 1 | **VIDEO** | NLE — timeline, preview, captions, export | MEDIA, EDIT, CAPTIONS, EXPORT |
| 2 | **FUSION** | Node compositor — OFX FX graph, keyframes | EDIT (graph view) |
| 3 | **3D-CGI** | Low-poly 3D scene — software rasterizer | SESSION (scene view) |
| 4 | **AGI** | AI task orchestration — render/polish/cut | CAPTIONS (task panel) |
| 5 | **PERF** | Live performance — VJ deck grid | SESSION (deck view) |

---

## The Tabs

### AUDIO Domain (tier 0)
- **ARRANGE** — linear timeline, track lanes, clip blocks, mixer, param editor
- **PAD** — 4×8 launchpad for auditioning notes/instruments
- **STEP** — 16×8 step sequencer with per-step velocity/probability
- **SESSION** — Ableton-style clip launcher (scenes × tracks)

### VIDEO Domain (tier 1)
- **MEDIA** — file browser, import, proxy generation (480p)
- **EDIT** — clip editor, trim mode, razor, slide/slip, JKL shuttle
- **CAPTIONS** — transcript editor, SRT round-trip, burn captions
- **EXPORT** — render queue, delivery presets, codec choice

### FUSION/CGI/AGI/PERF (tiers 2-5)
- **EDIT** tab hosts the active tier's view when FUSION/CGI/AGI/PERF is selected
- Each tier draws its own panel: node graph, 3D scene, task list, or deck grid

---

## Engine Modules (src/wb_*.c)

The engine is organized as **self-contained C11 modules**. Each `.c` file is a
domain. Headers in `include/wbus/` expose the public API. Internal decls live in
`wb_internal.h`.

### Core
| Module | Purpose |
|--------|---------|
| `wb_core.c` | Engine: transport, graph, mixer, command queue |
| `wb_transport.c` | Sample-accurate sequencer/timeline |
| `wb_session.c` | Session model: tracks, clips, notes, save/load |
| `wb_cmd.c` | Lock-free SPSC command queue (UI→engine) |
| `wb_backend.c` | Audio backend dispatch: CoreAudio + offline file |

### Audio DSP
| Module | Purpose |
|--------|---------|
| `wb_dsp.c` | DSP registry + unit lifecycle |
| `wb_osc.c` | Oscillators + mipmapped wavetables |
| `wb_env.c` | ADSR envelope |
| `wb_filter.c` | Biquad EQ / SVF filters |
| `wb_comp.c` / `wb_comp_simd.c` | Compressor/limiter (scalar + SIMD) |
| `wb_reverb.c` / `wb_conv_reverb.c` | FDN reverb + convolution reverb |
| `wb_delay.c` | Delay / echo |
| `wb_synth.c` / `wb_sampler.c` | Subtractive polysynth + sample playback |
| `wb_eq.c` / `wb_dynamic_eq.c` | Parametric EQ + dynamic EQ |
| `wb_ladder.c` | Moog ladder filter |
| `wb_phaser.c` | Phaser |
| `wb_chorus.c` | Chorus |
| `wb_wah.c` | Auto-wah |
| `wb_fuzz.c` | Big Muff fuzz |
| `wb_karplus.c` | Karplus-Strong physical modeling |
| `wb_drum_machine.c` / `wb_drum_rack.c` | TR-808/909 + drum rack |
| `wb_saturation.c` / `wb_sat_simd.c` | Tanh saturation (scalar + SIMD) |
| `wb_exciter.c` | Harmonic exciter |
| `wb_deesser.c` | De-esser |
| `wb_transient_shaper.c` | Transient shaper |
| `wb_stereo.c` / `wb_stereo_image.c` | M/S widening + stereo imaging |
| `wb_spatial_audio.c` | Surround/spatial monitoring |
| `wb_sidechain.c` / `wb_lfo_sidechain.c` | Sidechain + LFO sidechain |
| `wb_parallel_comp.c` | Parallel compression |
| `wb_multiband.c` | Multiband dynamics |
| `wb_limiter.c` / `wb_true_peak.c` | Limiter + true peak |
| `wb_lufs.c` | LUFS metering (K-weighted) |
| `wb_yin.c` | YIN pitch detection |
| `wb_pitch_correct.c` | Auto-tune pitch correction |
| `wb_pitch_bend.c` | Pitch bending |
| `wb_timestretch.c` | WSOLA time-stretch |
| `wb_warp.c` | Audio warping |
| `wb_granular.c` | Granular synthesis |
| `wb_formant.c` | Formant filtering |
| `wb_vocal_remove.c` | Vocal removal |
| `wb_vocal_synth.c` | Vocal synthesis |
| `wb_tuner.c` | Instrument tuner |
| `wb_freeze.c` | Track freeze |
| `wb_expression.c` | Expression parsing |
| `wb_quantize.c` | MIDI quantize |
| `wb_mpe.c` | MPE (MIDI Polyphonic Expression) |
| `wb_hpss.c` | Harmonic/percussive source separation |
| `wb_spectral_edit.c` / `wb_spectral_fx.c` | Spectral editing + FX |
| `wb_restoration.c` / `wb_restore.c` | Audio restoration |
| `wb_noise_gate.c` (via `wb_gate.c`) | Noise gate |
| `wb_dynamics_adv.c` | Advanced dynamics |
| `wb_mastering_chain.c` / `wb_mastering_pro.c` | Mastering chain |
| `wb_bass_boost.c` | Bass boost |
| `wb_tape_stop.c` | Tape stop effect |
| `wb_vca.c` | VCA |
| `wb_macro_rack.c` | Macro rack |
| `wb_mod.c` / `wb_mod_matrix.c` | Modulation + mod matrix |
| `wb_fm.c` / `wb_fm_g2.c` / `wb_fm_g3.c` | FM synthesis (3 operators) |
| `wb_sf2.c` | SoundFont 2 loader |
| `wb_drums.c` | Drum synthesis |
| `wb_bitcrush.c` | Bitcrusher |
| `wb_bleep.c` | Bleep/censor |
| `wb_clap.c` / `wb_unit_clap.c` | CLAP plugin support |
| `wb_midi_generators.c` / `wb_midi_humanize.c` / `wb_midi_scale.c` / `wb_midi_chordgen.c` / `wb_midi_remote.c` / `wb_midifx.c` | MIDI tools |
| `wb_arpeggiator.c` | Arpeggiator |
| `wb_pattern.c` | Pattern sequencing |
| `wb_beat_sync.c` | Beat-sync editing |
| `wb_beat_slicer.c` | Beat slicing |
| `wb_tempo_detect.c` | Tempo detection |
| `wb_chord_detect.c` | Chord detection |
| `wb_audio_to_midi.c` | Audio-to-MIDI |
| `wb_melody_ai.c` / `wb_chord_ai.c` / `wb_arrange_ai.c` / `wb_ai_mix.c` | AI-assisted music |
| `wb_analysis.c` | Audio analysis |
| `wb_sonogram.c` | Sonogram |
| `wb_spectrum.c` | Spectrum visualizer |
| `wb_waveview.c` | Waveform view |
| `wb_wavcache.c` | WAV cache |
| `wb_wavetable.c` | Wavetable management |
| `wb_perf.c` / `wb_perfclip.c` | Performance engine |
| `wb_track_folder.c` | Track folders |
| `wb_linked_tracks.c` | Linked tracks |
| `wb_pdc.c` | Plugin delay compensation |
| `wb_automation.c` | Automation lanes |
| `wb_param_track.c` | Param keyframe tracks |
| `wb_keyframes.c` | Keyframe system |
| `wb_project.c` / `wb_project_templates.c` | Project + templates |
| `wb_session_file.c` / `wb_session_view.c` | Session file I/O + views |
| `wb_workspace.c` | Workspace tier controller |
| `wb_undo.c` | Undo/redo |
| `wb_delivery.c` | Delivery profiles |
| `wb_export_job.c` / `wb_export_queue.c` | Background render queue |
| `wb_export_presets.h` | Export presets |
| `wb_proxy.c` | Proxy management |
| `wb_import.c` | Media import |
| `wb_input.c` | Audio input capture |
| `wb_capture.c` | Screen/camera capture |
| `wb_recorder.c` | Recording |
| `wb_bg_render.c` | Background rendering |
| `wb_cloud.c` / `wb_cloud_collab.c` | Cloud + collaboration |
| `wb_graphio.c` | Graph I/O |
| `wb_score.c` | Score/notation |
| `wb_keys.c` | Keyboard shortcuts |
| `wb_time_sig.c` | Time signature |
| `wb_subtitle_translate.c` | Subtitle translation |
| `wb_aaf_export.c` | AAF export |
| `wb_sfx.c` | Sound effects |
| `wb_atmos.c` | Ambient/atmosphere |
| `wb_audio_color.c` | Audio color mapping |
| `wb_autoreframe.c` | Auto-reframe |
| `wb_color_grading.c` | Color grading |
| `wb_text_templates.c` / `wb_text_edit.c` / `wb_text_animate.c` | Text tools |
| `wb_anim.c` | Animation |
| `wb_lottie.c` | Lottie animation |
| `wb_rast.c` | Software rasterizer |
| `wb_mesh.c` | Mesh primitives |
| `wb_gltf.c` | glTF loader |
| `wb_csg.c` | Constructive solid geometry |
| `wb_tga.c` | TGA image I/O |
| `wb_lut.c` | LUT support |
| `wb_light2d.c` | 2D lighting |
| `wb_reaction.c` | Reaction system |
| `wb_particle.c` / `wb_particle_gpu.c` | Particle system |
| `wb_char2d.c` | 2D character rigging |
| `wb_bvh.c` | BVH motion capture parser |
| `wb_motion_track.c` | Motion tracking |
| `wb_kaleidoscope.c` | Kaleidoscope effect |
| `wb_stabilize.c` / `wb_stabilize2.c` | Video stabilization |
| `wb_scene_detect.c` | Scene detection |
| `wb_scenedesc.c` | Scene description |
| `wb_shadowbin.c` | ShadowBin (portable edit state) |
| `wb_precision.c` | Precision editing |
| `wb_ftz.c` | FTZ/DAZ |
| `wb_dsp_simd.h` / `wb_biquad_simd2.h` / `wb_fir_simd.h` / `wb_branchless.h` / `wb_ringbuf.h` / `g2_fm_simd.h` / `sse_sin.h` | SIMD/optimization headers |
| `wb_vst3_host.cpp` | VST3 host |
| `wb_ofx.c` / `wb_ofx_plugin_builtin.c` | OFX plugin host |

### Video / YTP
| Module | Purpose |
|--------|---------|
| `wb_video.c` | FFmpeg C API decoder + proxy + preview |
| `wb_video_edit.c` | Video editing (scene detect, trim, etc.) |
| `wb_vfx.c` | Video FX: deep fry, VHS, shake, zoom, chromatic aberration, etc. |
| `wb_ytp.c` | YTP audio FX: stutter, pitch, earrape, reverse, sentence mix, datamosh, vine boom |
| `wb_stutter.c` | 10 stutter variations |
| `wb_deep_fry.c` | Deep fry multi-pass |
| `wb_vhs_effect.c` | VHS tracking lines, color bleed, static |
| `wb_speed_ramp.c` | Smooth speed curves |
| `wb_audio_reactive.c` | Bass zoom, beat flash, color shift |
| `wb_transitions.c` / `wb_transitions_pro.c` | Video transitions |
| `wb_compositor.c` / `wb_compositor_pro.c` | Node compositor |
| `wb_chroma_key.c` / `wb_chromakey.c` | Chroma key |
| `wb_datamosh.c` | Datamosh/pixel corruption |
| `wb_meme_sounds.c` | Meme sound effects |
| `wb_captions.c` | Caption generation + burn |
| `wb_transcript.c` | Transcript model |
| `wb_auto_captions.c` | Auto-captioning |
| `wb_lyric_video.c` | Lyric video generation |
| `wb_podcast.c` | Podcast pipeline |
| `wb_voice_polish.c` | Voice polishing (two-pass BS.1770) |
| `wb_voice_isolate.c` | Voice isolation |
| `wb_tts.c` | Text-to-speech |
| `wb_cgiexport.c` | CGI overlay export |

### Compositor / CGI
| Module | Purpose |
|--------|---------|
| `wb_compositor.c` / `wb_compositor_pro.c` | Pull-based node compositor (Natron-style) |
| `wb_cgi.c` / `wb_cgi_bands.c` / `wb_cgi_react.c` | 3D-CGI scene model |
| `wb_rast.c` | Software rasterizer (Jet-style fixed-function) |
| `wb_mesh.c` | Mesh primitives + OBJ loader |
| `wb_gltf.c` | glTF 2.0 loader |
| `wb_anim.c` | Keyframed animation |
| `wb_cgiexport.c` | CGI→video overlay export |

### AI / Agent
| Module | Purpose |
|--------|---------|
| `wb_agent.c` | Agent command protocol (stdin/pipe) |
| `wb_agi.c` | AGI task orchestration model |
| `wb_melody_ai.c` / `wb_chord_ai.c` / `wb_arrange_ai.c` / `wb_ai_mix.c` | AI music assistance |
| `wb_scenedesc.c` | Scene description (AGI→CGI bridge) |

---

## The Agent API

Big Mac exposes a **line-oriented command protocol** (`wb_agent_run`) that lets an
AI agent drive the editor without the GUI. This is the "agent use" path — it reuses
the REAL session/export/EDL APIs so there's no second code path.

**Protocol** (one command per line, `#` = comment):
```
import <src> [proxy]        add a video clip to the session
split  <track> <clip> <t>   split a clip at timeline seconds t
quality <0..1>              set the proxy/QoS dial
polish <src> <out> <lufs>   voice-polish a WAV -> out WAV
edl  <out.edl>              export CMX3600 EDL
fcpxml <out.xml>            export Final Cut XML
export <out.mp4> [srt]      render session -> mp4
quit                        stop reading
```

**AGI task orchestration** (`wb_agi`) is the in-app task queue: submit tasks like
"render episode", "polish voice", "auto-cut shorts" — each gets a status
(queued/running/done/failed) and progress. The AGI workspace tier renders the task
list. The agent bridge (`wb_agent_set_perf`) wires the DAW's live performance into
the agent's control surface.

---

## YTP / Meme Production

YTP is **one tab in the suite**, not a standalone tool. It uses the engine's
existing video pipeline:

```
Source video → wb_video.c (decode) → wb_vfx.c (FX) → wb_compositor.c (composite)
              → wb_ytp.c (audio FX) → wb_video_export_codec (export)
```

**Engine modules for YTP:**
- `wb_ytp.c` — audio: stutter loop, pitch shift, earrape, reverse, sentence mix, datamosh, vine boom
- `wb_vfx.c` — video: deep fry, VHS, shake, zoom, chromatic aberration, RGB glitch, posterize, vignette
- `wb_stutter.c` — 10 stutter variations
- `wb_deep_fry.c` — multi-pass deep fry
- `wb_vhs_effect.c` — VHS tracking lines, color bleed, static
- `wb_speed_ramp.c` — smooth speed curves
- `wb_audio_reactive.c` — bass zoom, beat flash, color shift
- `wb_datamosh.c` — pixel corruption between frames
- `wb_meme_sounds.c` — meme sound effects
- `wb_scene_detect.c` — scene change detection (auto-cut points)
- `wb_bvh.c` — BVH motion capture (dancing skeleton overlay)
- `wb_char2d.c` — 2D character rigging

**Tools (orchestration, NOT replacement):**
- `ytp_transcribe` — video → whisper.cpp → word-level JSON
- `wb_ytp_studio` — context-aware director (4-act plot structure)
- `wb_ytp_director` — text description → EDL
- `wb_ytp_compose` — transcript → edit structure
- `wb_ytp_render` — batch compositor
- `wb_mocap_overlay` — BVH skeleton overlay on video
- `wb_sentence_mix` — word-level sentence mixing

**Encoding spec:** 480p, CRF 28, ultrafast, 64k audio, faststart.

---

## Session Model

Everything shares ONE `wb_session`:

```
wb_session
  ├── tracks[] (audio + video, kind field distinguishes)
  │     └── clips[] (audio clips OR video clips)
  │           ├── notes[] (MIDI, for audio clips)
  │           └── wb_video_clip (for video clips)
  ├── markers[] (arrangement markers)
  ├── automation lanes[] (breakpoint curves over song time)
  ├── chord grid[] (harmonic transformation)
  ├── bin[] (media bin entries)
  └── project (multi-timeline container)
```

**Key rules:**
- `wb_clip` layout is FROZEN (memcpy'd/.wbus-loaded). Per-clip metadata goes in
  the `wb_clip_edit` side-table keyed by (track, clip).
- `wb_note` IS extensible (text save format).
- Audio clips use SAMPLES for start/length. Video clips use SECONDS. Don't mix them.
- Video track kind = 3 (`WB_TRACK_KIND_VIDEO`), bus = 2.

---

## Build & Gate

```bash
make clean && make            # 0 compile errors
./build/wb_selftest           # engine gate (read count from output)
make test_<module>            # per-module gate
make test_export_e2e          # must print E2E_EXPORT_OK
```

**All counts are LIVE** — read from the build, never from docs.
Run `bash tools/bigmac_status.sh` for a full snapshot.

---

## Repo Layout

```
big-mac/
├── Makefile                    # build + test gate
├── AGENTS.md                   # agent context (repo-level)
├── docs/
│   ├── WIKI.md                 # this file — the suite overview
│   ├── R072-giant-gap-research.md  # gap ledger (all 95 WIRED)
│   ├── R083-research-and-build-session.md
│   └── ...                     # research docs per round
├── include/wbus/               # public API headers
│   ├── wbus.h                  # engine, transport, session
│   ├── wbus_video.h            # video track/decode/proxy
│   ├── wbus_compositor.h       # node compositor
│   ├── wbus_workspace.h        # workspace tier system
│   ├── wbus_agent.h            # agent command protocol
│   ├── wbus_agi.h              # AGI task orchestration
│   ├── wbus_cgi.h              # 3D-CGI scene
│   └── ...                     # one header per domain
├── src/wb_*.c                  # engine modules (one domain each)
├── tools/
│   ├── wb_daw.c                # the app: SDL2 UI + engine + backend
│   ├── wb_render.c             # CLI offline render
│   ├── wb_selftest.c           # headless engine self-test (the gate)
│   ├── test_*.c                # per-module gate tests
│   ├── wb_ytp_studio.c         # YTP context-aware director
│   ├── wb_ytp_director.c       # YTP text→EDL
│   ├── wb_ytp_compose.c        # YTP transcript→edit
│   ├── wb_ytp_render.c         # YTP batch compositor
│   ├── wb_mocap_overlay.c      # BVH skeleton overlay
│   ├── ytp_transcribe.c        # video→whisper→JSON
│   ├── wb_sentence_mix.c       # word-level mixing
│   └── bigmac_status.sh        # live-count snapshot script
├── tests/test_*.c              # additional test modules
├── assets/
│   ├── ytp_sources/video/      # source videos for YTP
│   └── mocap/bandai_namco/     # BVH motion capture files
└── third_party/
    └── SDL2-2.32.10/           # vendored SDL2 (only third party)
```

---

## Design Rules (WuBu Doctrine)

1. **Pure C11.** No Rust, no C++ (except VST3 host shim). No third-party libs except vendored SDL2.
2. **Opaque structs.** Modules expose only their public API. Internals never leak.
3. **No god headers.** Each module includes only what it needs.
4. **One session model.** Audio and video share `wb_session`. No duplication.
5. **Engine modules, NOT standalone tools.** Features go in `src/wb_*.c`, wired into the UI via `tools/wb_daw.c`.
6. **Agent reuse.** The agent API (`wb_agent`) calls the SAME engine paths as the GUI. No second code path.
7. **Byte-exact honesty.** No faked output. If it says "rendered", it rendered.
8. **Match the machine.** Dual-core iMac. SIMD where it matters, but no waste.

---

## Status

Run `bash tools/bigmac_status.sh` for live counts. All 95 gap ledger items (R072) are WIRED.
