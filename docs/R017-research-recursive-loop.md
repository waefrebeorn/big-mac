# R017 — Recursive 7-Hop Research Loop: Gaps & Opportunities

**Method:** 4 parallel research subagents, each ran a 7-hop Kevin Bacon across
≥6 sources / ≥3 domains (25+ sources total, 7 domains). Goal: find what Big
Mac's video+audio editor is *missing* vs the best-in-class, and confirm the
architecture bets already made in R013–R016. This is the recursive
improvement loop: know where you are by knowing where you aren't.

**Domains covered:** Natron, Olive, MLT/Shotcut/Kdenlive, Blender VSE, MoviePy,
CasparCG, Resolume (compositor/NLE); Audacity/Tenacity, Ardour, Hindenburg/
Descript, Opus Clip/Riverside/CapCut, OpenCut (audio + AI); VapourSynth,
AVISynth, VirtualDub, GStreamer, mpv, Godot/Unity, DaVinci (realtime/render);
OFX, VST3, CLAP, OTIO/EDL/CMX3600/FCPXML/AAF, ffmpeg lavfi, ASS/SSA/SRT,
Lottie, glTF (interop/extensibility).

---

## Convergent truth (one line)

> Every mature editor collapses to a **single host-driven, pull-on-demand node
> graph** with **auto-inserted bounded edge caches**, a **proxy/quality dial
> negotiated by downstream QoS**, and a **GPU path that always retains a CPU
> fallback** — and it exposes **one normalized, time-addressed parameter/event
> bus** that the same automation timeline drives for audio plugins (VST3/CLAP),
> video FX (OFX), and compositor nodes alike.

Big Mac is already pointed at exactly this (R013 pull compositor + keyframe
tracks + VST3/CLAP automation). The research says: **the bet is right; now
close the remaining gaps below.**

---

## Gap → Action table (ranked by leverage on a dual-core iMac)

| # | Gap (where we aren't) | Evidence (source) | Action for Big Mac | Priority |
|---|------------------------|-------------------|--------------------|----------|
| G1 | ✅ DONE — proxy-scale QoS dial: `wb_compositor_set_quality(0..1)` is a process-global knob that shrinks tile size on slow frames (1024px full → 128px draft), modeled on GStreamer QoS + Resolve render cache. | GStreamer QoS (`proportion`+`jitter`) [T3-2]; Resolve Render Cache [T3-6] | `wb_compositor_set_quality`/`get_quality`/`tile_size` + agent 'quality' hook + test (in test_agent) | P1 |
| G2 | ✅ DONE — auto-insert LRU cache at graph edges: `wb_graph_auto_cache(root)` walks the graph and wraps every non-source child in a bounded `wb_node_cache` (AVISynth internal caching / Natron per-node hash-cache), idempotent. Verified: inserted ≥2 caches, second identical pull is a cache HIT (memoized, output correct). | AVISynth internal caching intercepts `GetFrame` between nodes [T3-3]; Natron hash-cache per node [T1-1] | `wb_graph_auto_cache` + `wb_node_cache_stats` + test 50/50 (G2 block) | P1 |
| G3 | ✅ DONE — two-phase pull: `wb_node_pull_request` (phase 0) schedules decodes so slow sources run ahead, then `wb_node_pull` (phase 1) computes (VapourSynth arInitial→arAllFramesReady). `wb_node_decode_source` models an async decoder (request-before-compute). Verified: request sets pending, no frame yet; compute returns frame. | VapourSynth `arInitial`→`arAllFramesReady` [T3-1] | `wb_node_pull_request` + `wb_node_decode_source` + test 50/50 (G3 block) | P2 |
| G4 | ✅ DONE — minimal OpenFX host: Property/Parameter/ImageEffect/Memory/TimeLine suites implemented; loads OfxPlugin via OfxGetPlugin, runs Load→Describe→CreateInstance→Render→DestroyInstance. Builtin Brightness plugin proves the contract: identity at brightness 1.0, 2x at 2.0, keyframed "Brightness" track (G11 bus) drives it. OFX node wraps as wb_node in the pull-compositor. | OFX `GetRegionOfDefinition`/`clipGetImage`/`IsIdentity` [T4-1] | `wb_ofx.c` + `wb_ofx_plugin_builtin.c` + `test_ofx` 13/13 | P1 |
| G5 | ✅ DONE — EDL/FCPXML interchange: `wb_session_export_edl` (CMX3600) + `wb_session_export_fcpxml` serialize all video clips (reel/src-in-out, rec-in-out) so projects travel to Resolve/Premiere/Final Cut. Verified by agent test (2 clips → 2 EDL events + 2 FCPXML assets + spine). | OTIO `adapters.read/write_from_file` (edl,fcpxml,aaf) [T4-4]; CMX3600 reel/char limits [T4-5] | `wb_session_export_edl`/`fcpxml` + agent 'edl'/'fcpxml' + test_agent | P2 |
| G6 | ✅ DONE — editable `wb_transcript` word model ([start,end,word]) with click-to-seek (`word_at`), drag-select, edit, SRT parse + round-trip; `wb_captions_get_transcript_model` bridges whisper SRT | Descript wordbar; Hindenburg auto-level [T2]; OpenCut plugin-first [T2] | `wb_transcript` (parse SRT→words) + `test_transcript` 18/18 | P1 |
| G7 | ✅ DONE — voice-polish is a param-track-driven graph: each stage (gate/deess/comp/limiter/EQ/lufs) bindable to a keyframed `wb_param_track` via `wb_voice_polish_bind`; `param_at` reads track-or-static per sample-time. Verified: keyframed comp_ratio 1→8 changes output (peak 0.35→0.59). | FFmpeg `loudnorm` two-pass + `afftdn`/`deesser`/`dynaudnorm` as composable nodes [T2-1,2] | `wb_voice_polish` (bind/set/param_at) + test 12/12 | P2 |
| G8 | ✅ DONE — EBUR128 two-pass loudness: `wb_voice_polish_apply_twopass` measures input K-weighted loudness (BS.1770) FIRST, then applies chain + single linear gain to target. Verified: input -31.9 → -16.6 LUFS (two-pass) vs -20.1 (single-pass); no clip. | FFmpeg `loudnorm` two-pass I/K-weighted [T2-2]; EBUR128 [T5-2] | `wb_voice_polish_apply_twopass` + test (15/15) | P2 |
| G9 | ✅ DONE — agent / MCP-style headless API: `wb_agent_run(FILE*, session, engine)` drives the editor via a line protocol (import/split/quality/edl/fcpxml/export/polish) reusing the real session/export/EDL/voice-polish paths. Verified 17/17 (script → session model + EDL + FCPXML + two-pass polish). | OpenCut MCP server for AI agents [T2]; OpenFX passive plugins [T1] | `wbus_agent.h` + `wb_agent.c` + test_agent 17/17 | P2 |
| G10 | ✅ DONE — ASS styled-caption support: `wb_ass_extract_dialogue` parses `Dialogue:` lines with inline overrides `\b \i \c&HBBGGRR& \pos(x,y) \move(...)` (ASS color is BGR→RGB), and `wb_captions_burn_ass` burns an .ass file via ffmpeg `subtitles=` (full ASS styling). Verified 14/14 (times, bold/italic flags, color BGR→RGB, pos parsed, text correct). | ASS `Dialogue:` + overrides [T4-7] | `wb_ass_extract_dialogue` + `wb_captions_burn_ass` + test_captions 14/14 | P3 |
| G11 | ✅ DONE — keyframe `wb_param_track` AND session `wb_automation_lane` both drive compositor FX node params (one bus) | VST3 `IParamValueQueue`; CLAP `clap_event_param_value` sample-offset [T4-2,3] | Unified via `wb_node_add_param` (track) + `wb_node_add_param_lane` (session lane); verified 33/33 checks, animates FX output | P1 |
| G12 | ✅ DONE — GPU-offload boundary: CPU path stays authoritative; `wb_compositor_set_backend(WB_BACKEND_CPU|GPU)` + `wb_frame.gpu` flag mark the pixel-buffer interop boundary where a Metal layer can wrap `wb_px`. Verified: backend flag set/get, frame marked GPU-eligible, CPU output unchanged under GPU flag. | mpv `--hwdec-software-fallback` [T3-4]; Godot tile memory [T3-8] | `wb_compositor_set_backend`/`get_backend` + `wb_frame_set_gpu` + test 50/50 (G12 block) | P3 |

---

## What the research *confirmed* (not gaps — validated bets)

- **Pull model (R013 D1/D3):** Natron `renderRoI` recursion + Olive `getValueAt`
  + MoviePy `get_frame(t)` all confirm ONE recursive `pull(t,roi)`. ✅ implemented.
- **Hash cache:** Olive `FrameHashCache` (no dirty flags) + Natron `Hash64`.
  ✅ `wb_node_cache` does content-hash LRU.
- **Identity short-circuit:** VirtualDub bypass shunt + OFX `IsIdentity`.
  ✅ `wb_node_effect` op=0 passes through.
- **Proxy transparency:** Blender `seq_proxy_fetch` + MLT proxy wrapper.
  ✅ `wb_video_clip` swaps proxy↔full-res at export.
- **Keyframe valid-clamp:** Olive `KeyframeTrack`. ✅ `wb_param_track` clamps
  outside keyed range by default.
- **One param bus:** VST3/CLAP automation flags + OFX params. ✅ `wb_param_track`
  is the shared channel; only G11 (binding) remains.

---

## Recursive loop verdict

This is iteration **N**. Where we *were* (start of session): video editor was
half-wired, export was a stub, `^X`/`^D` were TODOs, no voice-polish, no
keyframes, no compositor. Where we *are* now: all of that is implemented and
**verified by a clean build + 6 test gates (214 checks, 0 failures)**.

Where we *aren't* (next loop, from G1–G12): the editor is a working
timeline+export tool but **not yet a Fusion-class node compositor with
third-party OFX effects, EDL interchange, transcript editing, or an agent API.**
Those are G4, G5, G6, G9 — the highest-leverage next steps.

**Recommendation for loop N+1:** G11 is now CLOSED — the param bus is live
(compositor FX params ride `wb_param_track` + session `wb_automation_lane`).
Next highest-leverage: **G4** (minimal OFX host — now trivial since the
compositor already speaks RoI/tile/identity and params ride the shared bus)
and **G6** (transcript editing — whisper captions + the lane bus = click-word
to-seek + drag-to-trim fall out naturally).
