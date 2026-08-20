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
| Node compositor: **G1 proxy/QoS dial** — `wb_compositor_set_quality(0..1)` shrinks tile size on slow frames | `make test_agent` (quality 0.25 → tile 352; 1.0 → 1024) |
| Session: **EDL + FCPXML interchange** (G5) — CMX3600 + Final Cut XML of video clips | `make test_agent` (2 clips → 2 EDL events + 2 FCPXML assets) |
| Voice-polish: **EBUR128 two-pass** (G8) — measure input, linear apply to target | `make test_voice_polish` (input -31.9 → -16.6 LUFS) |
| Agent API: headless line protocol (G9) import/split/quality/edl/fcpxml/export/polish | `make test_agent` (17/17) |
| Node compositor: **G11 unified param bus** — FX node params driven by keyframed `wb_param_track` OR session `wb_automation_lane` (same channel) | `make test_compositor` (33/33: keyframed gain animates FX, lane bus clamps, lane→FX binding) |
| Node compositor: **G2 auto-insert edge cache** — `wb_graph_auto_cache(root)` wraps every non-source node in a bounded LRU `wb_node_cache` | `make test_compositor` (50/50: ≥2 caches inserted, repeated pull is a memoized hit) |
| Node compositor: **G3 two-phase pull** — `wb_node_pull_request` (phase 0) schedules decodes; `wb_node_pull` (phase 1) computes; `wb_node_decode_source` models async decode | `make test_compositor` (50/50: request sets pending, no frame; compute returns frame) |
| Captions: **ASS styled-caption parser + burn** (G10) — `wb_ass_extract_dialogue` parses `\b \i \c&HBBGGRR& \pos \move` (BGR→RGB); `wb_captions_burn_ass` burns via ffmpeg `subtitles=` | `make test_captions` (14/14: times, bold/italic, color, pos, text) |
| Node compositor: **G12 GPU-offload boundary** — `wb_compositor_set_backend(CPU|GPU)` + `wb_frame.gpu` flag; CPU path authoritative, Metal interop slot | `make test_compositor` (66/66: backend set/get, frame GPU-eligible, CPU output unchanged, R018-B color nodes) |
| Video: **ProRes editorial export** (R018-A) — `wb_video_export_codec` with `WB_VIDEO_CODEC_PRORES(_HQ)` (prores_ks 10-bit 4:2:2) for NLE interchange | `make test_export_e2e` (ProRes .mov reports `prores` codec via ffprobe) |
| Voice: **spectral voice isolation** (R018-D) — `wb_voice_isolate` STFT + per-bin noise-floor Wiener gate (RNNoise-style, no ML weights); self-contained `wb_fft` radix-2 | `make test_voice_isolate` (8/8: FFT round-trip, noise RMS reduced, tone preserved, no NaN) |
| Voice: **live gated loudness meter** (R018-G) — `wb_loudness_meter` streaming BS.1770-4 gated integrated + short-term LUFS (Resolve/Fairlight meter parity) | `make test_loudness_meter` (6/6: sane range, tracks level) |
| Compositor: **HDR / wide-gamut color pipeline** (R018-B) — `wb_node_colorspace` (sRGB↔linear, PQ/HLG HDR, Rec.709↔2020) + `wb_node_tonemap` (Reinhard/ACES) on the 32-bit-float frame | `make test_compositor` (66/66: CST round-trips, PQ peak, gamut matrix, tonemap bounds) |
| Interchange: **FCPXML intent enrichment** (R018-C) — `<adjust-color>` (exposure/saturation) on video clips + `audioRole` + `<adjust-volume>` on audio; "transfer intent, not just data" | `make test_fcpxml` (10/10: adjust-color values, role, volume carried) |
| **Voice: in-repo TTS engine** (R019) — `wb_tts` pure-C11 formant/articulatory synth (Klatt-style), dictionary + rule G2P, no model download, zero-dep, 22.05 kHz; neural ggml-VITS backend architected (deferred, needs vendored model) | `make test_tts` (14/14: offline, deterministic, non-silent, no NaN, WAV write) |
| **Audio: harmonic-percussive source separation** (R020-A) — `wb_hpss` classic two-stage median-filter HPSS (Driedger/Soros): steady tonal bed vs transients, pure C11, no ML weights; gives the DAW a stem split (melody/bass vs drums) | `make test_hpss` (7/7: split pad->harmonic, clicks->percussive, finite, energy retained) |
| **Video: transform node (Ken Burns / zoom-punch)** (R020-B) — `wb_node_transform` affine scale/pan/rotate with keyframable params on the G11 bus, so clips animate (zoom-in) without re-encoding | `make test_compositor` (72/72: 4x zoom pushes corner off-frame, keyframed zoom interpolates) |
## R017 recursive-loop gaps — ALL 12 CLOSED ✅ (G1–G12)
| Gap | Status |
|---|---|
| G1 proxy/QoS dial · G2 auto edge cache · G3 two-phase pull · G4 OFX host · G5 EDL/FCPXML · G6 transcript edit · G7 param-track graph · G8 EBUR128 two-pass · G9 agent API · G10 ASS captions · G11 unified param bus · G12 GPU boundary | ✅ all verified by `make test_*` (see Wired table above) |

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
