# Big Mac DAW — Wired/Open Ledger

## Cloned vendored dependency
- `third_party/SDL2-2.32.10/` — SDL2 2.32.10 built from source as a static
  library. **The only third-party; everything else is ours.** Chosen for
  portable windowing + portable audio across macOS/Linux/Windows.

## Reference sources studied
- `~/ref/ardour/` — Ardour (`libs/ardour/audioengine.cc`, `graph.cc`)
- `~/ref/lmms/` — LMMS (`src/core/AudioEngine.cpp`, `midi/MidiApple.cpp`)
  Lessons applied (R002): staged render (schedule→instruments→effects), Xrun
  try-lock, DAG worker model.
- `~/ref/natron/` — Natron `main` (shallow, 82M). Open-source node compositor
  (Fusion/Nuke-class). Engine: `EffectInstance.cpp` (pull `getImagePlane`,
  `resolveRoIForGetImage` RoI/RoD), `TreeRender.h` (render coordinate),
  `ImageCacheKey.h` (tile cache). Studied for R013 — the "Fusion sauce".
- `~/ref/openfx/` — OpenFX SDK `main` (shallow, 24M). `include/` (spec headers:
  ofxCore.h, ofxImageEffect.h), `HostSupport/examples/hostDemo*` (reference
  host), `Examples/Invert|Basic` (reference plugins). The plugin standard
  Fusion/Resolve/Nuke all implement; SLERM target for `wb_ofx_host`.
- `third_party/SDL2-2.32.10/` — SDL2 2.32.10 vendored source (101M). Design:
  thin cross-platform abstraction over native media APIs (cocoa/coreaudio/...).
  Studied in place for R014 P2/P10.
- `~/ref/olive/` — Olive video editor `master` (shallow, 50M). C++/Qt, **node-based
  compositing** + timeline. `app/node/` (pull `NodeTraverser`), `app/render/
  framehashcache.h` (per-time frame cache — validates R013 D3), `precachetask.h`.
  Studied for R015 — confirms/extends the node direction.
- `~/ref/olive/` (R015/R016), `~/ref/opencut/` (R015/R016), `~/ref/lossless-cut/` (R016,
  ✅ source-read: `-c copy` concat trim + scene/black/silence detect),
  plus 16 more acquired (read pending): `shotcut`, `kdenlive`, `openshot-qt`,
  `flowblade`, `vidcutter`, `moviepy`, `editly`, `FFCreator`, `openshorts`,
  `audacity`, `tenacity`, `pyJianYingDraft` (and prior `ardour`, `lmms`, `natron`,
  `openfx`). **19 reference repos total in `~/ref/`, all outside the repo.** Study
  + SLERM to C11; never link/vendor their code.

## Wired (verified by `make test`)
| Capability | Verify |
|---|---|
| Engine boot + transport (sample-accurate) | `make test` |
| Lock-free SPSC command queue (UI→engine) | `make test` |
| Session model (2-track, clips, notes, save/load) | `make test` |
| DSP graph + mixer (pan, gain, mute/solo) | `make test` |
| Subtractive synth (osc/env/filter, 16 voices) | `make test` |
| Compressor, delay, reverb, sampler | `make test` |
| WAV write (16-bit PCM + 32-bit float) | `make test` |
| Offline render (oracle parity with live) | `make test` |
| Recursive learn/fix feedback loop | `make test` (tuner converges) |
| Staged render pipeline (schedule→instr→fx) | `make test` |
| Xrun detection + counter (try-lock, never blocks) | `make test` |
|| Insert FX chain (comp/reverb/delay per track) | `make test` |
|| Per-slot bypass toggle + wet/dry mix on every insert slot (track & bus) | `make test` (test_bypass_wet + test_compressor_sidechain verify per-slot state + parallel wet signals + key-input ducking) |
| CoreMIDI input (enumerate + open by name) | launched, opened "Launchpad MK2" |
| Text UI (5×7 bitmap font: labels/numbers) | `build/wb_daw` shows time/BPM/dB |
| Video editor: session video track/clip model (add track/clip, set proxy, hit-test, remove) | `make test_video` (28 checks: track-kind, clip lifecycle, duration resolve, hit-test, remove) |
| Video editor: lossless keyframe trim + segment detect (scene/black/silence) | `make test_video` (concat demuxer `-c copy`, pure-ffmpeg detect) |
| Video editor: end-to-end export (engine audio render → ffmpeg mux → mp4 w/ video+audio) | `make test_export_e2e` (E2E_EXPORT_OK, ffprobe: video+audio streams) |
| Video editor: DaVinci-style 4-tab UI (MEDIA/EDIT/CAPTIONS/EXPORT) + import/proxy/captions/export keys | `build/wb_daw` (tabs 5–8; ^I import, ^G captions, ^R export, ^S set path, ^B burn) |
| Video editor: split clip (^X) — one clip → two, source-window preserved | `make test_video_tools` (left+right len == orig, src offset shifts) |
| Video editor: auto clip-to-shorts (scene-detect → lossless multi-export) | `make test_video_tools` (3 shorts from test src) |
| Video editor: transcript model (word-level [start,end,word], click-to-seek, drag-trim, SRT round-trip, captions bridge) (R017 G6) | `make test_transcript` (18/18) |
| Voice-polish: gate→deesser→comp→EQ→limiter→BS.1770 loudness preset (R015 T1) | `make test_voice_polish` (8/8: -10.7→-16 LUFS, sib -11dB, no clip) |
| Voice-polish: **param-track-driven graph** (G7) — each stage bindable to keyframed `wb_param_track` via the shared bus | `make test_voice_polish` (12/12: keyframed comp_ratio 1→8 changes output) |
| Keyframe tracks: hold/linear/bezier + valid-clamp (R016 S2) | `make test_compositor` (shared param bus for FX/automation) |
| Node compositor: recursive pull(t,roi), RoI→RoD, identity bypass, content-hash LRU edge cache (R013 D1/D3) | `make test_compositor` (18/8: composite/identity/cache-hit) |
| Node compositor: **minimal OpenFX host** (G4) — loads OfxPlugin, wraps as wb_node, keyframed param rides G11 bus | `make test_ofx` (13/13: Load→Describe→Render, identity, 2x, keyframe) |
| Node compositor: **G11 unified param bus** — FX node params driven by keyframed `wb_param_track` OR session `wb_automation_lane` (same channel) | `make test_compositor` (33/33: keyframed gain animates FX, lane bus clamps, lane→FX binding) |

## Open (next) — recursive-loop gaps (see R017 G1–G12)
| Capability | Source / why |
|---|
| **G11** Bind `wb_param_track` to FX + audio-plugin automation (one param bus) | VST3/CLAP/OFX all share normalized param channel (R017) — cheapest unification, unblocks G4/G6 |
| **G4** Minimal OFX host runner (RoI/tile + IsIdentity) to load Fusion/Resolve/Nuke effects | OFX `GetRegionOfDefinition`/`GetRegionOfInterest` (R017) |
| **G1** Proxy-scale QoS dial: `wb_compositor_set_quality` swaps proxy↔full-res + tile size on slow frames | GStreamer QoS, Resolve Render Cache (R017) |
| **G2** Auto-insert `wb_node_cache` at every graph edge (bounded LRU) | AVISynth internal edge cache (R017) |
| **G6** Transcript-editable timeline: click word→seek, drag→trim (whisper already done) | Descript wordbar, Hindenburg auto-level (R017) |
| **G5** EDL/CMX3600/FCPXML import-export (thin adapter; OTIO spine later) | OTIO adapters, CMX3600 reel limits (R017) |
| **G9** Agent/MCP batch API on `wb_daw` (headless drive) | OpenCut MCP server (R017) |
| **G3** Two-phase pull (request inputs → compute) for async decode | VapourSynth arInitial→arAllFramesReady (R017) |
| **G7** Voice-polish as param-track-driven pluggable graph (tunable stages) | FFmpeg composable filter nodes (R017) |
| **G10** ASS override-token parser + styled burn (currently SRT only) | ASS `Dialogue:` + `\pos \move \b \i` (R017) |
| **G12** GPU-offload boundary (Metal interop), CPU path authoritative | mpv hwdec-software-fallback (R017) |

## Research docs
- `R006-launchpad-ux-research.md` — 7-hop Kevin Bacon (25 sources, 8 domains): convergent truth = one owned surface, scale-locked, color-coded, no menu-diving.
- `R007-launchpad-mk2-driver-and-tabs.md` — applied spec: Mk2 byte-exact protocol, C11 driver API, tabbed UI, scale lock, verification plan, build order.
- `R008-video-editor-research.md` — 7-hop: Sony-Vegas-style assembly editor on FFmpeg + our audio engine; 480p proxy; auto-captions via FFmpeg Whisper; 1080p60 export.
- `R009-video-editor-design.md` — implementation spec: video track/clip model, FFmpeg C-API decode, proxy, captions, export, SDL2 UI, build order.
- `R010`–`R012` — whisper model selection + C11 ASR slot + SLERM-whisper design.
- `R013-fusion-equivalent-compositor-sauce.md` — **DaVinci Resolve / Fusion source is PROPRIETARY (not downloadable).** Recovered the real sauce via Natron (open Fusion-twin) + OpenFX SDK: pull-based RoI/RoD node graph, tile cache, OFX host/plugin action contract. Gives the build order to lift the video editor to Fusion/Resolve standard (D1–D6).
- `R014-design-principles.md` — consolidated design language across SDL + DAW + video editor + compositor: P1–P12 (staged/non-blocking, device-independence, pull eval, RoI/RoD, tile cache, render coordinate, passive plugins, user-owned graph, one-surface/color=state, local-first/SLERM, seconds-vs-samples boundary, verify-by-running).
- `R015-editor-landscape-research.md` — 7-hop Kevin Bacon across the editor landscape: Olive (node graph + frame hash cache), OpenCut (CapCut twin), Shotcut (native/no-import), Kdenlive (proxy+VST), LosslessCut (-c copy trim), Descript (transcript editing), Hindenburg (voice polish), Reaper (routing matrix), Bitwig (modular), Opus/Riverside/CapCut (AI auto-clip/reframe). Convergent truth: the best editors ELIMINATE a manual translation step. Adoption matrix + 3-tier "make ours the best" roadmap.
- `R016-editor-landscape-source-digest.md` — acquire the FULL landscape as local source in `~/ref/` (language-agnostic; SLERM to C11): olive, opencut, lossless-cut (source-read) + 13 more acquired. Sauce from source: Olive typed-input pull graph + keyframe tracks (lin/hold/bezier+valid-clamp) + frame hash cache; LosslessCut concat-demuxer `-c copy` trim + scene/black/silence detect; OpenCut plugin-first + MCP/agent API. Convergent: pull node graph + lossless trim + agent-API is THE standard. Concrete C11 SLERM target table.
- `R017-research-recursive-loop.md` — RECURSIVE 7-hop × 25-source Kevin Bacon loop (4 parallel subagents, 7 domains) converging gaps vs best-in-class. Convergent truth = one host-driven pull graph + auto edge cache + proxy QoS dial + GPU-with-CPU-fallback + one normalized param bus. 12 ranked gaps G1–G12 with build order; validates R013/R016 bets.

Pinned: R002 wiring (staged render/Xrun/double-buffer/DAG-worker model) is done — next is the mixing topology above, not more RT-pattern work. Research R006/R007 scopes the Launchpad Mk2 + tabbed-UI push.
